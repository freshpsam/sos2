/* Native WCN3620 backend. Firmware is provisioned offline; the trusted desktop
 * owns network selection and private persistent credentials. */
#include "redmi2_wifi.h"
#include "redmi2_rails.h"
#include "network.h"
#include "wifi_setup.h"
#include "wcn_hal.h"
#include "wcnss_power.h"
#include "runtime_files.h"
#include "platform.h"
#include "entropy.h"
#include "kernel_io.h"
#include "wifi/port.h"
#include "mbedtls/hmac_drbg.h"
static unsigned char firmware_memory[WCNSS_MEMORY_SIZE] __attribute__((aligned(1048576)));
static unsigned char metadata[QCOM_MDT_METADATA] __attribute__((aligned(4096)));
static struct wcn_dma dma;
static struct wcnss_boot boot;
static struct wcn_hal hal;
static struct wcn_dxe dxe;
static struct wifi_station station;
static struct wifi_profile profile;
static struct smd_channel channel;
static struct smem_region shared;
static struct wcnss_io hardware;
static bool enabled, failed, opened, station_started, stop_pending, linked;
static bool servicing, rng_ready;
static bool restart_requested, survey_requested, survey_resume;
static bool radio_ready, startup_complete;
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
static volatile bool standby_wanted;
static unsigned request_id, running_id, network_count, scan_generation;
static struct wifi_network networks[WIFI_NETWORK_MAX];
static struct net_device *network_device;
static const char *failure_reason;
static struct mbedtls_hmac_drbg_context auth_random;
static uint64_t next_service, channel_deadline;

static struct {
    unsigned size;
    unsigned char data[1514];
} received[8];

static unsigned receive_head, receive_count, receive_dropped;
static unsigned discard_base;
static unsigned char random_pool[512];
static unsigned random_head, random_count;
static bool random_wanted;

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

static void fail(const char *why) {
    if (failed) return;
    failed = true;
    linked = false;
    stop_pending = enabled;
    failure_reason = why;
    /* Destruction runs on the serialized worker after any suspended crypto
     * operation returns, never from a main-loop network callback. */
}

static bool online(void) { return enabled && !failed && hal.ready; }

static bool link(void) { return online() && !restart_requested && station.state == WIFI_ONLINE; }

static void stop(void) { fail("network stopped"); }

static bool send_frame(const void *data, unsigned size) {
    if (servicing || !link() || !wifi_station_send(&station, data, size)) return false;
    ++network_device->tx_packets;
    return true;
}

static int configure(void *p, enum wifi_action action, const struct wifi_station *s) {
    (void)p;
    return wcn_hal_configure(&hal, action, s, time_us());
}

static bool send_wire(void *p, const void *data, unsigned size, bool is_data) {
    (void)p;
    return wcn_hal_send(&hal, &dxe, data, size, is_data, time_us());
}

static void ethernet(void *p, const void *data, unsigned size) {
    (void)p;
    if (size > 1514 || receive_count == 8) {
        ++receive_dropped;
        return;
    }
    unsigned at = (receive_head + receive_count) % 8;
    memcpy(received[at].data, data, size);
    received[at].size = size;
    ++receive_count;
}

static void receive_wire(void *p, const unsigned char *data, unsigned size,
                         const struct wcn_rx_info *info) {
    (void)p;
    struct wifi_rx_status status = {info->decrypted, info->management, info->key_index,
                                    info->data_offset};
    wifi_station_receive_checked(&station, data, size, &status, time_us());
}

static bool drained(void *p) {
    (void)p;
    return wcn_dxe_drained(&dxe);
}

static uint64_t clock_now(void *p) {
    (void)p;
    return time_us();
}

static void discovered(void *p, const struct wifi_network *network) {
    (void)p;
    for (unsigned i = 0; i < network_count; ++i)
        if (networks[i].ssid_length == network->ssid_length &&
            networks[i].modes == network->modes &&
            !memcmp(networks[i].ssid, network->ssid, network->ssid_length))
            return;
    if (network_count < WIFI_NETWORK_MAX) networks[network_count++] = *network;
}

static int seed_random(void *p, unsigned char *out, size_t size) {
    (void)p;
    return entropy_read(out, size) == size ? 0 : -1;
}

static int authentication_random(void *p, unsigned char *out, size_t size) {
    (void)p;
    if (!rng_ready || !servicing || !entropy_available() ||
        mbedtls_hmac_drbg_random(&auth_random, out, size))
        return -1;
    kernel_io_wait();
    return failed ? -1 : 0;
}

static void poll(void (*receive)(const void *, unsigned)) {
    /* lwIP stays on the main loop. Only bounded copies cross from the radio
     * worker, so expensive SAE/entropy operations cannot freeze the shell. */
    if (link())
        network_device->rx_discarded = dxe.dropped + station.rx_data_seen -
                                       station.rx_data_admitted + receive_dropped - discard_base;
    network_device->tx_status = wcn_hal_tx_status(&hal);
    while (receive_count) {
        if (link()) {
            ++network_device->rx_packets;
            receive(received[receive_head].data, received[receive_head].size);
        }
        received[receive_head].size = 0;
        receive_head = (receive_head + 1) % 8;
        --receive_count;
    }
}

static bool start_station(uint64_t now, bool survey) {
    struct wifi_ops ops = {.configure = configure,
                           .send = send_wire,
                           .receive = ethernet,
                           .random = authentication_random,
                           .drained = drained,
                           .clock = clock_now,
                           .discovered = discovered};
    bool result = survey ? wifi_station_survey(&station, profile.mac, &ops, now)
                         : wifi_station_start(&station, &profile, &ops, now);
    if (!result) {
        fail("wireless profile unsupported");
        return false;
    }
    station_started = true;
    running_id = request_id;
    restart_requested = false;
    return true;
}

static void radio_poll(void) {
    if (!enabled || failed || boot.state != WB_RUNNING) return;
    uint64_t now = time_us();
    if (!opened) {
        if (!channel_deadline) channel_deadline = now + 5000000;
        struct smem_item info, fifo;
        int result = smem_smd_find(&shared, 6, "WLAN_CTRL", &info, &fifo);
        if (result < 0 || (!result && now >= channel_deadline)) {
            fail("WLAN control channel unavailable");
            return;
        }
        if (!result) return;
        if (!smd_open(&channel, info.data, info.size, fifo.data, fifo.size, wlan_signal, 0, now)) {
            fail("WLAN control channel cannot be attached");
            return;
        }
        opened = true;
        channel_deadline = 0;
        if (!wcn_hal_start(&hal, &channel, boot.nv.data, (unsigned)boot.nv.size, profile.mac)) {
            fail("WLAN firmware initialization rejected");
            return;
        }
    }
    if (!wcn_hal_poll(&hal, now)) {
        fail(hal.error);
        return;
    }
    if (!hal.ready) return;
    if (profile.security && !rng_ready) {
        if (!entropy_available() ||
            mbedtls_hmac_drbg_seed(&auth_random, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                   seed_random, 0, profile.mac, 6)) {
            fail("wireless secure randomness unavailable");
            return;
        }
        mbedtls_hmac_drbg_set_reseed_interval(&auth_random, 256);
        rng_ready = true;
        now = time_us();
    }
    if (!radio_ready) {
        /* SMSM discovery gets its own budget after HAL and entropy startup. */
        if (!channel_deadline) channel_deadline = now + 5000000;
        struct smem_item smsm;
        int result = smem_get(&shared, 85, &smsm);
        if (result < 0 || (result == 1 && smsm.size < 32)) {
            fail("invalid SMSM wireless state");
            return;
        }
        if (!result) {
            if (now >= channel_deadline) fail("SMSM wireless state unavailable");
            return;
        }
        volatile uint32_t *apps = (volatile void *)smsm.data;
        uint32_t value = *apps;
        *apps = (value & ~0x600u) | 0x200;
        barrier();
        hardware.signal(hardware.context, 1u << 19);
        if (!wcn_dxe_start(&dxe, &dma, (uintptr_t)&dma, &hardware)) {
            fail("wireless DMA initialization failed");
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
    if (!station_started) {
        /* An unconfigured desktop stays idle until Scan Networks is tapped.
         * A configured connection still performs its required directed scan. */
        if (!restart_requested) return;
        if (!survey_requested && !profile.ssid_length) {
            restart_requested = false;
            return;
        }
        bool survey = survey_requested;
        survey_resume = survey && profile.ssid_length;
        survey_requested = false;
        if (survey) {
            network_count = 0;
            ++scan_generation;
        }
        if (!start_station(now, survey)) return;
    }
    /* Handle the control hint first: newer queued RX can cancel its probe. */
    if (hal.beacon_missed) {
        hal.beacon_missed = false;
        wifi_station_beacon_missed(&station, now);
    }
    if (!wcn_dxe_poll(&dxe, receive_wire, 0, now)) {
        fail("wireless DMA fault or timeout");
        return;
    }
    now = time_us(); /* Received authentication frames can also yield for crypto. */
    if (hal.link_lost) {
        if (hal.sta_lost) station.sta_live = false;
        hal.link_lost = hal.sta_lost = false;
        wifi_station_reconnect(&station, "firmware reported link loss", now);
    }
    if (restart_requested) {
        if (station.state == WIFI_BACKOFF || station.state == WIFI_IDLE) {
            /* Recovery has acknowledged deletion of the old station/BSS and
             * drained DMA before any profile or authentication state changes. */
            if (!survey_requested && !profile.ssid_length) {
                /* Forgotten or failed without a saved network: also wipe the
                 * station's copy of the old PMK/password. */
                wifi_secret_clear(&station.profile, sizeof(station.profile));
                restart_requested = survey_resume = station_started = false;
                station.state = WIFI_IDLE;
                return;
            }
            bool survey = survey_requested;
            survey_resume = survey && profile.ssid_length;
            survey_requested = false;
            if (survey) {
                network_count = 0;
                ++scan_generation;
            }
            if (!start_station(time_us(), survey)) return;
        } else wifi_station_reconnect(&station, "network selection changed", time_us());
    } else if (station.survey && station.state == WIFI_IDLE && survey_resume) {
        survey_resume = false;
        if (!start_station(time_us(), false)) return;
    }
    wifi_station_standby(&station, standby_wanted, time_us());
    wifi_station_poll(&station, time_us());
    if (station.state == WIFI_FAILED) {
        fail(station.error);
        return;
    }
    if (!linked && link()) {
        linked = true;
        discard_base =
            dxe.dropped + station.rx_data_seen - station.rx_data_admitted + receive_dropped;
    } else if (linked && !link()) {
        linked = false;
        receive_head = receive_count = 0;
    }
}

/* In standby the radio is off once the station has left its network (no
 * station, choosing a network, or waiting to rejoin), as Linux's wcn36xx
 * enters idle-mode power save for mac80211's idle state; otherwise, and
 * whenever awake, full power. */
static void radio_power(void) {
    if (!enabled || failed || !radio_ready || !hal.ready) return;
    bool off =
        standby_wanted && !restart_requested && !survey_requested && drained(0) &&
        (!station_started ||
         ((station.state == WIFI_IDLE || station.state == WIFI_BACKOFF) && !station.configuring));
    wcn_hal_power(&hal, off ? WCN_POWER_IDLE : WCN_POWER_FULL, time_us());
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

int redmi2_wifi_sleep_set_state(void) { return !enabled ? 1 : failed ? -1 : sleep_set_sent; }

bool redmi2_wifi_rpm_idle(void) { return !enabled || !boot.rpm.pending; }

bool redmi2_wifi_clock_scaling(void) { return enabled && boot.clock_scaling; }

void redmi2_wifi_standby(bool on) { standby_wanted = on; }

bool redmi2_wifi_standby_settled(void) {
    /* The sleep set goes out a request per poll or two, and the answer comes
     * back on one: 20 ms polls meanwhile. A failure's teardown (its secure
     * call among it) runs first, too. */
    if (stop_pending) return false;
    bool sending = power_released && sleep_set_wanted && !sleep_set_sent;
    return !enabled || failed || (hal.power == WCN_POWER_IDLE && !hal.power_request && !sending);
}

static void startup_progress(void *unused, const char *message) {
    (void)unused;
    wifi_setup_progress(message);
    /* Keep controls responsive between stages without painting each detail. */
    kernel_io_wait();
}

static bool start_radio(const unsigned char mac[6]) {
    if (enabled || failed || !wifi_mac_valid(mac)) return false;
    memcpy(profile.mac, mac, 6);
    memcpy(network_device->mac, mac, 6);
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
                          (uintptr_t)firmware_memory, metadata, (uintptr_t)metadata, time_us())) {
        failure_reason = boot.error;
        return false;
    }
    enabled = true;
    return true;
}

static unsigned connect_profile(const struct wifi_profile *next) {
    if (!enabled || failed) return 0;
    if (next) {
        unsigned char bytes[WIFI_PROFILE_BYTES];
        bool valid = wifi_profile_write(next, bytes);
        wifi_secret_clear(bytes, sizeof(bytes));
        if (!valid) return 0;
    }
    wifi_secret_clear(&profile, sizeof(profile));
    if (next) profile = *next;
    memcpy(profile.mac, network_device->mac, 6);
    if (!++request_id) ++request_id;
    restart_requested = true;
    survey_requested = survey_resume = false;
    receive_head = receive_count = 0;
    return request_id;
}

static void scan_networks(void) {
    if (enabled && !failed) {
        survey_requested = restart_requested = true;
        receive_head = receive_count = 0;
    }
}

static void snapshot(struct wifi_radio_view *out) {
    *out = (struct wifi_radio_view){
        .message = redmi2_wifi_status(),
        .detail = boot.firmware_detail[0] ? boot.firmware_detail : network_status_detail(),
        .count = network_count,
        .scan_generation = scan_generation,
        .request_id = running_id,
        .startup_pending = stop_pending || (enabled && !failed && !startup_complete)};
    memcpy(out->networks, networks, sizeof(networks));
    if (failed) out->state = WIFI_RADIO_FAILED;
    else if (!enabled) out->state = WIFI_RADIO_UNAVAILABLE;
    else if (!station_started) out->state = radio_ready ? WIFI_RADIO_IDLE : WIFI_RADIO_STARTING;
    else if (restart_requested || station.survey)
        out->state = station.state == WIFI_IDLE && !restart_requested ? WIFI_RADIO_IDLE
                                                                      : WIFI_RADIO_SCANNING;
    else if (link()) out->state = WIFI_RADIO_CONNECTED;
    else out->state = WIFI_RADIO_CONNECTING;
    out->attempt_failed = !restart_requested && !station.survey && !station.leaving &&
                          (station.state == WIFI_RECOVER || station.state == WIFI_BACKOFF);
    if (station_started && !station.survey) {
        out->current.ssid_length = station.profile.ssid_length;
        memcpy(out->current.ssid, station.profile.ssid, station.profile.ssid_length);
        out->current.modes = 1u << station.profile.security;
    }
}

static void cooperate(void *unused) {
    (void)unused;
    kernel_io_wait();
}

bool redmi2_wifi_init(const struct platform_devices *devices, struct net_device *out) {
    (void)devices;
    enabled = failed = opened = station_started = stop_pending = linked = servicing = rng_ready =
        false;
    next_service = channel_deadline = 0;
    power_release = (struct wcnss_power_release){0};
    power_released = 0;
    sleep_set_wanted = false;
    sleep_set_sent = 0;
    sleep_set_next = 0;
    sleep_set_deadline = 0;
    restart_requested = survey_requested = survey_resume = radio_ready = startup_complete = false;
    request_id = running_id = network_count = scan_generation = 0;
    network_device = out;
    receive_head = receive_count = receive_dropped = 0;
    failure_reason = 0;
    wifi_port_reset();
    wifi_port_set_yield(cooperate, 0);
    mbedtls_hmac_drbg_init(&auth_random);
    profile = (struct wifi_profile){0};
    random_head = random_count = 0;
    random_wanted = false;
    *out = (struct net_device){
        .online = online, .link = link, .poll = poll, .send = send_frame, .stop = stop};
    struct firmware_file config = lookup(0, "/system/wifi-profile");
    struct wifi_profile bootstrap;
    bool imported = wifi_profile_read(&bootstrap, config.data, config.size);
    const struct wifi_setup_ops ops = {
        start_radio,     redmi2_wifi_service_pending, redmi2_wifi_service, snapshot, scan_networks,
        connect_profile, wifi_auth_credential};
    wifi_setup_init(&ops, imported ? &bootstrap : 0);
    wifi_secret_clear(&bootstrap, sizeof(bootstrap));
    return true;
}

void redmi2_wifi_startup_timeout(void) { fail("Wi-Fi startup timed out"); }

size_t redmi2_wifi_random(void *out, size_t size) {
    random_wanted = true;
    if (size > random_count) size = random_count;
    for (unsigned i = 0; i < size; ++i)
        ((unsigned char *)out)[i] = random_pool[(random_head + i) % sizeof(random_pool)];
    for (unsigned i = 0; i < size; ++i) random_pool[(random_head + i) % sizeof(random_pool)] = 0;
    random_head = (random_head + size) % sizeof(random_pool);
    random_count -= size;
    return size;
}

bool redmi2_wifi_service_pending(void) {
    return stop_pending || (enabled && !failed && time_us() >= next_service) ||
           (enabled && !failed && random_wanted && random_count < 256 && entropy_available());
}

void redmi2_wifi_service(void *unused) {
    (void)unused;
    if (stop_pending) {
        wcn_dxe_stop(&dxe);
        if (opened) smd_close(&channel);
        wifi_station_stop(&station, failure_reason);
        wifi_secret_clear(hal.tx,
                          sizeof(hal.tx)); /* A queued key request may still hold key material. */
        mbedtls_hmac_drbg_free(&auth_random);
        rng_ready = false;
        wifi_crypto_wipe(random_pool, sizeof(random_pool));
        random_count = 0;
        wcnss_boot_stop(&boot);
        stop_pending = false;
        return;
    }
    if (enabled && !failed && time_us() >= next_service) {
        servicing = true;
        enum wcnss_boot_state state = wcnss_boot_poll(&boot, time_us());
        if (state == WB_FAILED) fail(boot.error);
        if (state == WB_RUNNING && !failed) {
            radio_poll();
            radio_power();
        }
        next_service = time_us() + 2000;
        servicing = false;
    }
    if (enabled && !failed && random_wanted && random_count < 256 && entropy_available()) {
        unsigned char bytes[256];
        size_t n = entropy_read(bytes, sizeof(bytes));
        for (unsigned i = 0; i < n; ++i)
            random_pool[(random_head + random_count + i) % sizeof(random_pool)] = bytes[i];
        random_count += n;
        volatile unsigned char *wipe = bytes;
        for (unsigned i = 0; i < sizeof(bytes); ++i) wipe[i] = 0;
        if (!n && !entropy_available()) fail("network entropy failed");
    }
    /* Fill both network queues before showing the desktop. Otherwise lwIP's
     * first polls trigger several slow jitter collections just after boot. */
    if (radio_ready && random_count >= 256 && !network_startup_pending()) startup_complete = true;
}

void redmi2_wifi_startup_progress(uint32_t *done, uint32_t *total) {
    /* Steps of 100: each WCNSS start state (the firmware copy in between),
     * then the HAL, the radio's DMA and the network's randomness pool. */
    enum { BOOT_STEPS = WB_RUNNING, STEPS = BOOT_STEPS + 3 };

    *total = STEPS * 100;
    if (failed || startup_complete) {
        *done = *total;
        return;
    }
    if (!enabled) {
        *done = 0;
        return;
    } /* Not started yet. */
    uint32_t value = boot.state > WB_RUNNING ? BOOT_STEPS * 100 : (uint32_t)boot.state * 100;
    if (boot.state == WB_COPY && boot.plan.span)
        value += (uint32_t)((uint64_t)boot.copied * 100 / boot.plan.span);
    if (boot.state == WB_RUNNING)
        value += (hal.ready ? 100 : 0) + (radio_ready ? 100 : 0) +
                 (random_count >= 256 ? 100 : random_count * 100 / 256);
    *done = value < *total ? value : *total;
}

const char *redmi2_wifi_status(void) {
    if (failed) return failure_reason ? failure_reason : "Wi-Fi hardware failed";
    if (!enabled) {
        if (failure_reason) return failure_reason;
        if (!entropy_available()) return "Wi-Fi blocked: secure randomness unavailable";
        const char *message = wifi_setup_view()->message;
        return message ? message : "Wi-Fi not started";
    }
    if (boot.state != WB_RUNNING) return boot.status ? boot.status : "Wi-Fi: preparing firmware";
    if (!hal.ready) return "Wi-Fi firmware configuring";
    if (!station_started) return radio_ready ? "Wi-Fi ready" : "Wi-Fi starting";
    if (restart_requested) return "Wi-Fi changing network";
    return wifi_station_status(&station);
}
