/* Native WCN3620 backend: the radio of the shared Wi-Fi device
 * (kernel/wifi/wifi_device.c). Firmware is provisioned offline; the trusted
 * desktop owns network selection and private persistent credentials. */
#include "redmi2_wifi.h"
#include "redmi2_rails.h"
#include "wifi_device.h"
#include "wifi_setup.h"
#include "wcn_hal.h"
#include "wcnss_power.h"
#include "runtime_files.h"
#include "platform.h"
#include "kernel_io.h"
static unsigned char firmware_memory[WCNSS_MEMORY_SIZE] __attribute__((aligned(1048576)));
static unsigned char metadata[QCOM_MDT_METADATA] __attribute__((aligned(4096)));
static struct wcn_dma dma;
static struct wcnss_boot boot;
static struct wcn_hal hal;
static struct wcn_dxe dxe;
static struct smd_channel channel;
static struct smem_region shared;
static struct wcnss_io hardware;
static bool opened, radio_ready;
/* Withdrawing the start-up power requests: 0 while under way, 1 done, -1 kept. */
static struct wcnss_power_release power_release;
static int power_released;
/* Whole-chip sleep's sleep set: wanted, sent (1) or failed (-1), next vote,
 * and the deadline to find room for it. */
static bool sleep_set_wanted;
static int sleep_set_sent;
static unsigned sleep_set_next;
static uint64_t sleep_set_deadline;
static void sleep_set_step(uint64_t now);
static uint64_t channel_deadline;

static struct firmware_file lookup(void *context, const char *path) {
    (void)context;
    const struct runtime_file *f = runtime_file_find(path);
    return f ? (struct firmware_file){runtime_file_data(f), f->size} : (struct firmware_file){0};
}

static uint32_t read_reg(void *p, uintptr_t address) {
    (void)p;
    return mmio_read32(address);
}

static void write_reg(void *p, uintptr_t address, uint32_t value) {
    (void)p;
    mmio_write32(address, value);
    barrier();
}

static void signal_remote(void *p, uint32_t mask) {
    (void)p;
    barrier();
    mmio_write32(0x0b011008, mask);
}

static bool lock(void *p) {
    (void)p;
    mmio_write32(0x01908000, 1);
    barrier();
    return mmio_read32(0x01908000) == 1;
}

static void unlock(void *p) {
    (void)p;
    barrier();
    mmio_write32(0x01908000, 0);
}

static void wlan_signal(void *p) {
    (void)p;
    signal_remote(0, 1u << 17);
}

static void receive_wire(void *p, const unsigned char *data, unsigned size,
                         const struct wcn_rx_info *info) {
    (void)p;
    struct wifi_rx_status status = {info->decrypted, info->management, info->key_index,
                                    info->data_offset};
    wifi_device_receive(data, size, &status);
}

static bool running(void) { return wifi_device_view()->enabled && !wifi_device_view()->failed; }

static void radio_poll(void) {
    if (!running() || boot.state != WB_RUNNING) return;
    uint64_t now = time_us();
    if (!opened) {
        if (!channel_deadline) channel_deadline = now + 5000000;
        struct smem_item info, fifo;
        int result = smem_smd_find(&shared, 6, "WLAN_CTRL", &info, &fifo);
        if (result < 0 || (!result && now >= channel_deadline)) {
            wifi_device_fail("WLAN control channel unavailable");
            return;
        }
        if (!result) return;
        if (!smd_open(&channel, info.data, info.size, fifo.data, fifo.size, wlan_signal, 0, now)) {
            wifi_device_fail("WLAN control channel cannot be attached");
            return;
        }
        opened = true;
        channel_deadline = 0;
        if (!wcn_hal_start(&hal, &channel, boot.nv.data, (unsigned)boot.nv.size,
                           wifi_device_mac())) {
            wifi_device_fail("WLAN firmware initialization rejected");
            return;
        }
    }
    if (!wcn_hal_poll(&hal, now)) {
        wifi_device_fail(hal.error);
        return;
    }
    if (!hal.ready || !wifi_device_seed()) return;
    now = time_us();
    if (!radio_ready) {
        /* SMSM discovery gets its own budget after HAL and entropy startup. */
        if (!channel_deadline) channel_deadline = now + 5000000;
        struct smem_item smsm;
        int result = smem_get(&shared, 85, &smsm);
        if (result < 0 || (result == 1 && smsm.size < 32)) {
            wifi_device_fail("invalid SMSM wireless state");
            return;
        }
        if (!result) {
            if (now >= channel_deadline) wifi_device_fail("SMSM wireless state unavailable");
            return;
        }
        volatile uint32_t *apps = (volatile void *)smsm.data;
        uint32_t value = *apps;
        *apps = (value & ~0x600u) | 0x200;
        barrier();
        hardware.signal(hardware.context, 1u << 19);
        if (!wcn_dxe_start(&dxe, &dma, (uintptr_t)&dma, &hardware)) {
            wifi_device_fail("wireless DMA initialization failed");
            return;
        }
        now = time_us(); /* DXE enable retries can yield through several redraws. */
        radio_ready = true;
        channel_deadline = 0;
    }
    /* The running firmware requests its own power: withdraw the start-up
     * requests, which kept the core rails at their highest voltage. */
    if (!power_released) power_released = wcnss_power_release(&power_release, &boot.rpm, now);
    if (power_released && sleep_set_wanted && !sleep_set_sent) sleep_set_step(now);
    if (!wifi_device_begin(now)) return;
    struct wifi_station *station = wifi_device_station();
    /* Handle the control hint first: newer queued RX can cancel its probe. */
    if (hal.beacon_missed) {
        hal.beacon_missed = false;
        wifi_station_beacon_missed(station, now);
    }
    if (!wcn_dxe_poll(&dxe, receive_wire, 0, now)) {
        wifi_device_fail("wireless DMA fault or timeout");
        return;
    }
    now = time_us(); /* Received authentication frames can also yield for crypto. */
    if (hal.link_lost) {
        if (hal.sta_lost) station->sta_live = false;
        hal.link_lost = hal.sta_lost = false;
        wifi_station_reconnect(station, "firmware reported link loss", now);
    }
    wifi_device_poll();
}

/* In standby the radio is off once the station has left its network (no
 * station, choosing a network, or waiting to rejoin), as Linux's wcn36xx
 * enters idle-mode power save for mac80211's idle state; otherwise, and
 * whenever awake, full power. */
static void radio_power(void) {
    if (!running() || !radio_ready || !hal.ready) return;
    wcn_hal_power(&hal, wifi_device_quiet() ? WCN_POWER_IDLE : WCN_POWER_FULL, time_us());
}

/* While the application processor sleeps with the RPM notified, the active
 * set holds unless a sleep set overrides it (rpm-smd-regulator.c): the kept
 * crystal vote would keep the crystal on. The sleep set drops it, and the
 * core rails' corner and enable, as the vendor's active-only votes do, and
 * the bus and memory clocks this processor holds awake (wcnss_boot.c), so
 * DDR can self-refresh, with the RPM's clock scaling on for this set too;
 * then the requests that lower the PMIC rails standby leaves idle
 * (rails.c). */
static void sleep_set_step(uint64_t now) {
    static const uint32_t votes[][4] = {
        {0x306b6c63, 0, 0x62616e45, 0},                                 /* XO source */
        {0x61706d73, 1, 0x6e726f63, 0}, {0x61706d73, 1, 0x6e657773, 0}, /* CX */
        {0x616f646c, 3, 0x6e726f63, 0}, {0x616f646c, 3, 0x6e657773, 0}, /* MX */
        {0x316b6c63, 0, 0x007a484b, 0}, {0x316b6c63, 1, 0x007a484b, 0}, /* PCNOC, SNOC, kHz */
        {0x326b6c63, 0, 0x007a484b, 0},                                 /* BIMC, kHz */
        {0x306b6c63, 2, 0x62616e45, 1}};                                /* clock scaling on */

    enum { VOTES = sizeof(votes) / sizeof(votes[0]), ROOM_US = 5000000 };
    struct rpm_request *rpm = &boot.rpm;
    if (rpm->failed) {
        sleep_set_sent = -1;
        return;
    }
    if (rpm->pending) {
        int result = rpm_poll(rpm, now);
        if (!result) return;
        if (result < 0) {
            sleep_set_sent = -1;
            return;
        }
        ++sleep_set_next;
    }
    /* The rails' requests follow, listed once Wi-Fi's are in (rails.c). */
    const struct redmi2_rail_request *rails = 0;
    unsigned total = VOTES + (sleep_set_next >= VOTES ? redmi2_rails_requests(&rails) : 0);
    if (sleep_set_next == total) {
        sleep_set_sent = 1;
        return;
    }
    /* No room to send for 5 s fails it, as the release gives up. */
    if (!sleep_set_deadline) sleep_set_deadline = now + ROOM_US;
    int result = -1;
    if (now < sleep_set_deadline && sleep_set_next < VOTES) {
        const uint32_t *v = votes[sleep_set_next];
        result = rpm_request(rpm, RPM_SLEEP, v[0], v[1], v[2], v[3], now);
    } else if (now < sleep_set_deadline) {
        const struct redmi2_rail_request *q = &rails[sleep_set_next - VOTES];
        result = rpm_request_kvps(rpm, q->set, q->type, q->id, q->kvp, q->n, now);
    }
    if (result > 0) sleep_set_deadline = 0;
    else if (result < 0) sleep_set_sent = -1;
}

void redmi2_wifi_sleep_set(void) { sleep_set_wanted = true; }

int redmi2_wifi_sleep_set_state(void) {
    const struct wifi_device_view *v = wifi_device_view();
    return !v->enabled ? 1 : v->failed ? -1 : sleep_set_sent;
}

bool redmi2_wifi_rpm_idle(void) { return !wifi_device_view()->enabled || !boot.rpm.pending; }

bool redmi2_wifi_clock_scaling(void) { return wifi_device_view()->enabled && boot.clock_scaling; }

bool redmi2_wifi_standby_settled(void) {
    /* The sleep set goes out a request per poll or two, and the answer comes
     * back on one: 20 ms polls meanwhile. A failure's teardown (its secure
     * call among it) runs first, too. */
    const struct wifi_device_view *v = wifi_device_view();
    if (v->stopping) return false;
    bool sending = power_released && sleep_set_wanted && !sleep_set_sent;
    return !v->enabled || v->failed ||
           (hal.power == WCN_POWER_IDLE && !hal.power_request && !sending);
}

static void startup_progress(void *unused, const char *message) {
    (void)unused;
    wifi_setup_progress(message);
    /* Keep controls responsive between stages without painting each detail. */
    kernel_io_wait();
}

static const char *start(const unsigned char mac[6]) {
    (void)mac;
    hardware = (struct wcnss_io){read_reg, write_reg, signal_remote, 0, startup_progress};
    shared = (struct smem_region){.base = (volatile void *)0x86300000,
                                  .size = 1024 * 1024,
                                  .lock = lock,
                                  .unlock = unlock,
                                  .physical = 0x86300000,
                                  .aux_physical = 0x60000,
                                  .aux = (volatile void *)0x60000,
                                  .aux_size = 0x8000};
    if (!wcnss_boot_begin(&boot, lookup, 0, &shared, &hardware, firmware_memory,
                          (uintptr_t)firmware_memory, metadata, (uintptr_t)metadata, time_us()))
        return boot.error ? boot.error : "Wi-Fi firmware unavailable";
    return 0;
}

static uint64_t service(void) {
    enum wcnss_boot_state state = wcnss_boot_poll(&boot, time_us());
    if (state == WB_FAILED) wifi_device_fail(boot.error);
    if (state == WB_RUNNING && running()) {
        radio_poll();
        radio_power();
    }
    return 2000;
}

static void stop(void) {
    wcn_dxe_stop(&dxe);
    if (opened) smd_close(&channel);
    /* A queued key request may still hold key material. */
    wifi_secret_clear(hal.tx, sizeof(hal.tx));
    wcnss_boot_stop(&boot);
}

static bool ready(void) { return radio_ready; }

static const char *starting(void) {
    if (boot.state != WB_RUNNING) return boot.status ? boot.status : "Wi-Fi: preparing firmware";
    return hal.ready ? 0 : "Wi-Fi firmware configuring";
}

static int configure(enum wifi_action action, const struct wifi_station *s, uint64_t now) {
    return wcn_hal_configure(&hal, action, s, now);
}

static bool send(const void *frame, unsigned size, bool data) {
    return wcn_hal_send(&hal, &dxe, frame, size, data, time_us());
}

static bool drained(void) { return wcn_dxe_drained(&dxe); }

static const struct wifi_radio radio = {.start = start,
                                        .service = service,
                                        .stop = stop,
                                        .ready = ready,
                                        .starting = starting,
                                        .configure = configure,
                                        .send = send,
                                        .drained = drained};

void redmi2_wifi_init(struct net_device *out) {
    opened = radio_ready = false;
    channel_deadline = 0;
    power_release = (struct wcnss_power_release){0};
    power_released = 0;
    sleep_set_wanted = false;
    sleep_set_sent = 0;
    sleep_set_next = 0;
    sleep_set_deadline = 0;
    wifi_device_init(&radio, out);
}

void redmi2_wifi_startup_progress(uint32_t *done, uint32_t *total) {
    /* Steps of 100: each WCNSS start state (the firmware copy in between),
     * then the HAL, the radio's DMA and the network's randomness pool. */
    enum { BOOT_STEPS = WB_RUNNING, STEPS = BOOT_STEPS + 3 };

    const struct wifi_device_view *v = wifi_device_view();
    *total = STEPS * 100;
    if (v->failed || v->startup_complete) {
        *done = *total;
        return;
    }
    if (!v->enabled) {
        *done = 0;
        return;
    } /* Not started yet. */
    uint32_t value = boot.state > WB_RUNNING ? BOOT_STEPS * 100 : (uint32_t)boot.state * 100;
    if (boot.state == WB_COPY && boot.plan.span)
        value += (uint32_t)((uint64_t)boot.copied * 100 / boot.plan.span);
    if (boot.state == WB_RUNNING)
        value += (hal.ready ? 100 : 0) + (radio_ready ? 100 : 0) +
                 (v->random_bytes >= 256 ? 100 : v->random_bytes * 100 / 256);
    *done = value < *total ? value : *total;
}
