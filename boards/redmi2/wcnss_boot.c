#include "wcnss_boot.h"
#include "qcom_scm.h"
#include "platform.h"
#include "wire.h"

enum {
    PMU = 0x0a21c004,
    COM_CSR = 0x0a21c040,
    SOFT_RESET = 0x0a21c04c,
    SPARE = 0x0a21c088,
    IRIS_READ = 0x0a21c134
};

/* Stock wt88047 wcnss_vreg.c: reset, read register 4, retry an absent ID up to
 * five times, then reset again before XO configuration. A completed PMU reset
 * alone does not establish that the RF chip answered. HAL's later NV status
 * codes are not a reliable substitute for this hardware identification. */
enum {
    IRIS_BASE = 0x30,
    IRIS_RESET = 1u << 7,
    IRIS_RESET_BUSY = 1u << 8,
    IRIS_READ_START = 1u << 9,
    IRIS_READ_BUSY = 1u << 10,
    IRIS_XO_START = 1u << 3,
    IRIS_XO_BUSY = 1u << 6,
    IRIS_ATTEMPTS = 5
};

/* msm8916.dtsi: Pronto watchdog is GIC SPI 149 (INTID 181). Observe the
 * pending latch while masked; never enable, acknowledge or clear another
 * driver's interrupt. An inherited pending bit is not a new radio fault. */
enum { WDOG_PENDING = 0x0b000214, WDOG_BIT = 1u << 21 };

/* Stock SMEM IDs are selected by the writer's base plus the reader's PID:
 * APPS (base 427, PID 0) -> WCNSS (PID 4), and WCNSS (base 451) -> APPS.
 * Only the APPS-owned item may be allocated, cleared or written here. */
enum { SMP2P_APPS_TO_WCNSS = 431, SMP2P_WCNSS_TO_APPS = 451, SMP2P_BYTES = 344 };

enum {
    NOTIFY_ABSENT = 0,
    NOTIFY_VALID = 1,
    NOTIFY_UNPUBLISHED = 2,
    NOTIFY_NO_ENTRY = 3,
    NOTIFY_BUSY = 4,
    NOTIFY_NEGOTIATING = 5
};

enum {
    CRYPTO_CMD = 0x01816004,
    CRYPTO_CFG = 0x01816008,
    CRYPTO_CORE = 0x0181601c,
    CRYPTO_AXI = 0x01816020,
    CRYPTO_AHB = 0x01816024,
    GPLL0_STATUS = 0x0182101c,
    SNOC_QOSGEN = 0x0182601c,
    PLL_VOTE = 0x01845000,
    BRANCH_VOTE = 0x01845004,
    CRYPTO_CFG_MASK = 0x10371f
};

static bool progress(struct wcnss_boot *b, const char *message) {
    if (b->status == message) return false;
    b->status = message;
    if (b->io.progress) b->io.progress(b->io.context, message);
    return true;
}

/* Start discovery budgets on their first actual poll, after status painting
 * and any preceding secure call. Hardware requests arm their own waits below. */
static void advance(struct wcnss_boot *b, enum wcnss_boot_state state) {
    b->state = state;
    b->deadline = 0;
}

static unsigned discovery_budget(const struct wcnss_boot *b) {
    switch (b->state) {
        case WB_RPM:
        case WB_SMP2P: return 5000000;
        /* Vendor subsystem_restart.c::wait_for_err_ready allows ten seconds. */
        case WB_READY_WAIT: return 10000000;
        case WB_CONTROL: return b->control_open ? 0 : 30000000;
        default: return 0;
    }
}

static bool clock_running(struct wcnss_boot *b, uintptr_t address) {
    uint32_t value = b->io.read(b->io.context, address);
    /* GCC branch2 also acknowledges an enabled, idle-gated NoC clock with
     * FSM_STATUS_ON (2), even while CLK_OFF is set. Other FSM states wait. */
    return !(value & (1u << 31)) || (value & 0x70000000) == 0x20000000;
}

static const char *stage(enum wcnss_boot_state state) {
    switch (state) {
        case WB_SCM: return "Wi-Fi: checking secure firmware";
        case WB_RPM: return "Wi-Fi: opening power controller";
        case WB_POWER: return "Wi-Fi: enabling power and RF clock";
        case WB_CLOCKS: return "Wi-Fi: enabling shared bus clocks";
        case WB_CLOCK_PLL: return "Wi-Fi: enabling crypto PLL";
        case WB_CLOCK_UPDATE: return "Wi-Fi: updating crypto clock";
        case WB_CLOCK_READY: return "Wi-Fi: enabling crypto bus clocks";
        case WB_METADATA: return "Wi-Fi: checking firmware metadata";
        case WB_MEMORY: return "Wi-Fi: reserving firmware memory";
        case WB_COPY: return "Wi-Fi: copying firmware";
        case WB_SMP2P:
        case WB_IRIS_RESET:
        case WB_IRIS_READ:
        case WB_IRIS_CONFIG:
        case WB_IRIS_XO:
        case WB_SETTLE: return "Wi-Fi: starting radio oscillator";
        case WB_AUTH: return "Wi-Fi: authenticating firmware";
        case WB_READY_WAIT: return "Wi-Fi: waiting for radio ready";
        case WB_CONTROL: return "Wi-Fi: sending board calibration";
        default: return "Wi-Fi: firmware ready";
    }
}

static const char *scm_error(const char *fallback) {
    const char *error = qcom_scm_error();
    return error ? error : fallback;
}

static void rpm_signal(void *p) {
    struct wcnss_boot *b = p;
    b->io.signal(b->io.context, 1);
}

static void wcn_signal(void *p) {
    struct wcnss_boot *b = p;
    b->io.signal(b->io.context, 1u << 17);
}

static bool crash_reason(struct wcnss_boot *b, char reason[81]) {
    struct smem_item item;
    enum smem_status result = smem_get_status(b->smem, 422, &item);
    memset(reason, 0, 81);
    if (result == SMEM_ABSENT) return true;
    if (result != SMEM_FOUND) return false;
    for (unsigned i = 0; i < 80 && i < item.size; ++i) {
        unsigned char c = item.data[i];
        if (!c) break;
        reason[i] = (char)c;
    }
    barrier();
    return true;
}

/* Linux's qcom_scm votes for the crypto engine's clocks only around each
 * secure-firmware call (qcom_scm_pas_init_image, _mem_setup, _auth_and_reset,
 * _shutdown) and drops the vote afterwards; the running WCNSS does not use
 * them. SOS2 does not use the crypto engine either. */
static void crypto_clocks(struct wcnss_boot *b, bool on) {
    uint32_t vote = b->io.read(b->io.context, BRANCH_VOTE);
    b->io.write(b->io.context, BRANCH_VOTE, on ? vote | 7 : vote & ~7u);
    barrier();
    for (unsigned i = 0; on && i < 100000; ++i)
        if (clock_running(b, CRYPTO_CORE) && clock_running(b, CRYPTO_AXI) &&
            clock_running(b, CRYPTO_AHB))
            break;
}

static enum wcnss_boot_state failed(struct wcnss_boot *b, const char *why) {
    b->error = why;
    if (b->pas_initialized) {
        progress(b, "Wi-Fi: stopping firmware after failure");
        crypto_clocks(b, true);
        (void)qcom_scm_pas_stop();
        b->authenticated = false;
        b->pas_initialized = false;
    }
    crypto_clocks(b, false);
    if (b->control_open) smd_close(&b->control_channel);
    b->state = WB_FAILED;
    progress(b, why);
    /* Shared APPS power votes may also sustain display/touch. Do not drop rails
     * without a system power broker. Firmware RAM stays quarantined for boot. */
    return b->state;
}

static enum wcnss_boot_state radio_failed(struct wcnss_boot *b, const char *fallback) {
    char reason[81];
    /* Capture before shutdown can erase the evidence. A failed baseline read
     * gives no basis for attributing an inherited crash to this boot. Keep the
     * actual failure visible: this buffer is initialized during a normal boot,
     * so a changed string alone does not establish a fatal/watchdog event. */
    if (b->crash_baseline && crash_reason(b, reason) && reason[0] &&
        strcmp(reason, b->prior_crash)) {
        /* This exact default is copied to SMEM 422 by the supplied firmware's
         * error-framework initialization, before it has reported any fault. */
        if (!strcmp(reason, "SFR Init: wdog or kernel error suspected."))
            strcpy(b->firmware_detail,
                   "Firmware error log initialized; no specific crash reason supplied.");
        else {
            const char prefix[] = "Firmware: ";
            unsigned n = sizeof(prefix) - 1;
            memcpy(b->firmware_detail, prefix, n);
            for (unsigned i = 0; reason[i] && n < sizeof(b->firmware_detail) - 1; ++i) {
                unsigned char c = (unsigned char)reason[i];
                b->firmware_detail[n++] = c >= 32 && c < 127 ? (char)c : '?';
            }
            b->firmware_detail[n] = 0;
        }
    }
    return failed(b, fallback);
}

static enum wcnss_boot_state radio_event(struct wcnss_boot *b, const char *event) {
    const char *phase = b->state == WB_READY_WAIT ? "waiting for radio ready"
                        : b->state == WB_CONTROL  ? "sending board calibration"
                                                  : "running";
    unsigned n = 0;
    const char *parts[] = {"WCNSS ", event, ": ", phase};
    for (unsigned i = 0; i < 4; ++i)
        for (const char *p = parts[i]; *p && n < sizeof(b->diagnostic) - 1; ++p)
            b->diagnostic[n++] = *p;
    b->diagnostic[n] = 0;
    return radio_failed(b, b->diagnostic);
}

void wcnss_boot_stop(struct wcnss_boot *b) {
    if (b && b->state != WB_OFF && b->state != WB_FAILED) failed(b, "wireless stopped");
}

bool wcnss_boot_begin(struct wcnss_boot *b, firmware_lookup lookup, void *context,
                      struct smem_region *smem, const struct wcnss_io *io, void *memory,
                      uintptr_t physical, void *metadata, uintptr_t metadata_physical,
                      uint64_t now) {
    if (!b) return false;
    *b = (struct wcnss_boot){.state = WB_FAILED, .error = "invalid wireless startup configuration"};
    if (!io || !io->read || !io->write || !io->signal || !smem || !memory || !metadata ||
        !physical || (physical & 0xfffff) || physical > UINT32_MAX - WCNSS_MEMORY_SIZE ||
        !metadata_physical || (metadata_physical & 4095) ||
        metadata_physical > UINT32_MAX - QCOM_MDT_METADATA || now > UINT64_MAX - 30000000)
        return false;
    b->io = *io;
    b->smem = smem;
    b->memory = memory;
    b->physical = physical;
    b->metadata = metadata;
    b->metadata_physical = metadata_physical;
    if (!qcom_mdt_prepare(lookup, context, &b->plan, metadata, QCOM_MDT_METADATA)) {
        b->error = "missing or invalid relocatable WCNSS firmware";
        return false;
    }
    b->nv = lookup(context, "/firmware/wcnss/WCNSS_qcom_wlan_nv.bin");
    if (!b->nv.data || b->nv.size < 5 || b->nv.size > 1024 * 1024) {
        b->error = "matching board NV data required";
        return false;
    }
    advance(b, WB_SCM);
    b->error = 0;
    return true;
}

static bool open_channel(struct wcnss_boot *b, struct smd_channel *c, unsigned edge,
                         const char *name, void (*signal)(void *), uint64_t now, int *result) {
    b->diagnostic[0] = 0;
    struct smem_item info, fifo;
    *result = smem_smd_find(b->smem, edge, name, &info, &fifo);
    if (*result != 1) {
        /* Preserve the exact failed validation before another lookup or PAS
         * cleanup can overwrite the shared-memory driver's diagnostic. */
        if (*result < 0) memcpy(b->diagnostic, b->smem->error, sizeof(b->diagnostic));
        return false;
    }
    const uint32_t states[] = {*(volatile uint32_t *)info.data,
                               *(volatile uint32_t *)(info.data + info.size / 2)};
    barrier();
    if (edge == 15 && smd_adopt_idle(c, info.data, info.size, fifo.data, fifo.size, signal, b))
        return true;
    if (!smd_open(c, info.data, info.size, fifo.data, fifo.size, signal, b, now)) {
        /* Layout discovery succeeded. Distinguish an inherited channel that
         * cannot be safely attached from a malformed SMEM heap/table. */
        const char *prefix =
            edge == 15 ? "RPM channel attach: states=" : "WCNSS channel attach: states=";
        unsigned n = (unsigned)strlen(prefix);
        memcpy(b->diagnostic, prefix, n);
        for (unsigned j = 0; j < 2; ++j) {
            if (j) b->diagnostic[n++] = '/';
            for (unsigned i = 0; i < 8; ++i)
                b->diagnostic[n++] = "0123456789abcdef"[(states[j] >> (28 - 4 * i)) & 15];
        }
        b->diagnostic[n] = 0;
        *result = -1;
        return false;
    }
    return true;
}

static uint32_t shared_word(volatile unsigned char *p) {
    uint32_t n = *(volatile uint32_t *)p;
    barrier();
    return n;
}

static void shared_put(volatile unsigned char *p, uint32_t n) {
    barrier();
    *(volatile uint32_t *)p = n;
}

static const char *timeout_error(enum wcnss_boot_state state) {
    switch (state) {
        case WB_SCM: return "WCNSS secure firmware query timed out";
        case WB_RPM: return "RPM channel discovery timed out";
        case WB_POWER: return "WCNSS power setup timed out";
        case WB_CLOCKS:
        case WB_CLOCK_PLL: return "WCNSS crypto PLL setup timed out";
        case WB_CLOCK_UPDATE: return "WCNSS crypto clock update timed out";
        case WB_CLOCK_READY: return "WCNSS crypto bus clocks timed out";
        case WB_METADATA: return "WCNSS metadata setup timed out";
        case WB_MEMORY: return "WCNSS memory setup timed out";
        case WB_COPY:
        case WB_SMP2P: return "WCNSS shared-memory setup timed out";
        case WB_IRIS_RESET:
        case WB_IRIS_CONFIG: return "WCNSS IRIS reset timed out";
        case WB_IRIS_READ: return "WCNSS radio detection timed out";
        case WB_IRIS_XO: return "WCNSS IRIS clock setup timed out";
        case WB_SETTLE:
        case WB_AUTH: return "WCNSS firmware start timed out";
        case WB_READY_WAIT: return "WCNSS ready notification timed out";
        default: return "WCNSS control startup timed out";
    }
}

static enum wcnss_boot_state value_timeout(struct wcnss_boot *b, const char *label,
                                           uint32_t value) {
    const char *message = timeout_error(b->state);
    unsigned n = 0, length = (unsigned)strlen(label);
    while (*message && n < sizeof(b->diagnostic) - length - 9) b->diagnostic[n++] = *message++;
    memcpy(b->diagnostic + n, label, length);
    n += length;
    for (unsigned i = 0; i < 8; ++i)
        b->diagnostic[n++] = "0123456789abcdef"[(value >> (28 - 4 * i)) & 15];
    b->diagnostic[n] = 0;
    return b->state == WB_READY_WAIT ? radio_failed(b, b->diagnostic) : failed(b, b->diagnostic);
}

static enum wcnss_boot_state absent_timeout(struct wcnss_boot *b) {
    /* Stock wcnss_wlan.c reads these common PMU registers in its boot-failure
     * dump, before touching/gating WLAN clocks. Snapshot before PAS shutdown;
     * do not reset or power-cycle the radio to obtain diagnostics. */
    char *p = b->diagnostic;
    const char prefix[] = "WCNSS ready timeout: no item CSR=";
    memcpy(p, prefix, sizeof(prefix) - 1);
    unsigned n = sizeof(prefix) - 1;
    const uint32_t values[] = {b->io.read(b->io.context, COM_CSR),
                               b->io.read(b->io.context, SOFT_RESET)};
    for (unsigned j = 0; j < 2; ++j) {
        if (j) {
            memcpy(p + n, " RST=", 5);
            n += 5;
        }
        for (unsigned i = 0; i < 8; ++i)
            p[n++] = "0123456789abcdef"[(values[j] >> (28 - 4 * i)) & 15];
    }
    p[n] = 0;
    return radio_failed(b, p);
}

static int signals(struct wcnss_boot *b, uint32_t *value) {
    struct smem_item in;
    enum smem_status result =
        smem_get_peer_status(b->smem, SMEM_HOST_WCNSS, SMP2P_WCNSS_TO_APPS, &in);
    if (result == SMEM_BUSY) return NOTIFY_BUSY;
    if (result != SMEM_FOUND) return result;
    if (in.size < 20) return -1;
    volatile unsigned char *p = in.data;
    /* The remote allocates this item before publishing its header. Version is
     * written last; a zero version is not a corrupt packet during first boot. */
    uint32_t version = shared_word(p + 4);
    if (!(version & 255)) return b->smp_seen ? -1 : NOTIFY_UNPUBLISHED;
    uint32_t hosts = shared_word(p + 8), total = shared_word(p + 12) & 65535;
    if (shared_word(p) != 0x504d5324 || hosts != 4) return -1;
    uint32_t negotiated = 1 | ((version & 255) == 1 ? (version & 256) : 256);
    if (b->smp_seen && (!b->smp_out.data || version != shared_word(b->smp_out.data + 4))) return -1;
    if (!b->smp_seen && b->smp_out.data) {
        if (shared_word(b->smp_out.data + 4) != negotiated) {
            shared_put(b->smp_out.data + 4, negotiated);
            b->io.signal(b->io.context, 1u << 18);
        }
    }
    /* As in vendor smp2p_do_negotiation, publish our supported offer and wait
     * for the peer to agree before using entries or restart flags. */
    if (version != negotiated) return NOTIFY_NEGOTIATING;
    b->smp_seen = true;
    /* The remote need not allocate our 16-entry layout. Stock scans total
     * capacity (including holes), clamped to complete entries in the item. */
    unsigned entries = total;
    if (entries > (in.size - 20) / 20) entries = (unsigned)(in.size - 20) / 20;
    bool found = false;
    uint32_t flags = shared_word(p + 16);
    for (unsigned i = 0; i < entries; ++i) {
        char name[16];
        for (unsigned j = 0; j < 16; ++j) name[j] = (char)p[20 + i * 20 + j];
        if (!memcmp(name, "slave-kernel", sizeof("slave-kernel"))) {
            *value = shared_word(p + 36 + i * 20);
            found = true;
            break;
        }
    }
    /* Restart acknowledgement depends on the negotiated header, even before
     * the remote has published entries. Do not make its progress depend on us. */
    if ((version & 256) && b->smp_out.data) {
        uint32_t out_flags = shared_word(b->smp_out.data + 16), ack = (flags & 1) << 1;
        if ((out_flags & 2) != ack) {
            shared_put(b->smp_out.data + 16, (out_flags & ~2u) | ack);
            b->io.signal(b->io.context, 1u << 18);
        }
    }
    return found ? NOTIFY_VALID : total ? NOTIFY_NO_ENTRY : NOTIFY_UNPUBLISHED;
}

enum wcnss_boot_state wcnss_boot_poll(struct wcnss_boot *b, uint64_t now) {
    if (!b) return WB_FAILED;
    if (b->state == WB_FAILED || b->state == WB_OFF) return b->state;
    if (now > UINT64_MAX - 30000000) return failed(b, "wireless clock overflow");
    uint32_t notification_value = 0;
    int notification = NOTIFY_ABSENT;
    if (b->authenticated) {
        uint32_t pending = b->io.read(b->io.context, WDOG_PENDING);
        if (pending != UINT32_MAX) {
            if (!(pending & WDOG_BIT)) b->watchdog_clear_seen = true;
            else if (b->watchdog_clear_seen) return radio_event(b, "watchdog");
        }
        notification = signals(b, &notification_value);
        if (notification < 0) return failed(b, "WCNSS notification layout invalid");
        if (notification == NOTIFY_VALID && (notification_value & 1))
            return radio_event(b, "fatal notification");
    }
    /* Publishing can yield for a slow scanout. Resume hardware work on a new
     * poll with a fresh timestamp, never arm a deadline from before that yield. */
    if (progress(b, stage(b->state))) return b->state;
    if (!b->deadline) {
        unsigned budget = discovery_budget(b);
        if (budget) b->deadline = now + budget;
    }
    enum wcnss_boot_state before = b->state;
    int result;
    switch (b->state) {
        case WB_SCM:
            if (!qcom_scm_init()) return failed(b, scm_error("secure firmware query failed"));
            advance(b, WB_RPM);
            break;
        case WB_RPM:
            if (!open_channel(b, &b->rpm_channel, 15, "rpm_requests", rpm_signal, now, &result)) {
                if (result < 0)
                    return failed(b, b->diagnostic[0] ? b->diagnostic
                                                      : "RPM shared-memory channel invalid");
                break;
            }
            if (!rpm_init(&b->rpm, &b->rpm_channel))
                return failed(b, "RPM channel initialization failed");
            advance(b, WB_POWER);
            break;
        case WB_POWER: {
            /* CX's stock range includes corner 6. Retain that ceiling, also used
             * for MX, rather than lower an inherited shared APPS vote without a
             * system power broker (the WCNSS-only CX minimum would be 4). */
            static const uint32_t votes[][4] = {
            {0x616f646c,3,0x6e726f63,6},{0x616f646c,3,0x6e657773,1}, /* MX */
            {0x61706d73,1,0x6e726f63,6},{0x61706d73,1,0x6e657773,1}, /* CX */
            /* Stock Pronto-v2 load votes use milliamps. L7 is shared by the
             * 10 mA IRIS XO and the 18 mA PAS proxy supply; retain both here. */
            {0x616f646c,7,0x7675,1800000},{0x616f646c,7,0x616d,28},{0x616f646c,7,0x6e657773,1}, /* PX/XO */
            {0x61706d73,3,0x7675,1300000},{0x61706d73,3,0x616d,100},{0x61706d73,3,0x6e657773,1}, /* RFA */
            {0x616f646c,9,0x7675,3300000},{0x616f646c,9,0x616d,515},{0x616f646c,9,0x6e657773,1}, /* PA */
            {0x616f646c,5,0x7675,1800000},{0x616f646c,5,0x616d,10},{0x616f646c,5,0x6e657773,1}, /* DIG */
            {0x306b6c63,0,0x62616e45,1}, /* XO source, distinct from its RF buffer */
            {0x616b6c63,5,0x6e657773,1}, /* RF_CLK2, 19.2 MHz */
            /* Not Wi-Fi's: this processor's bus clocks, as Linux's clk-smd-rpm
             * hands them over at boot (clk_smd_rpm_handoff, then
             * clk_smd_rpm_enable_scaling). Until this processor enables the
             * RPM's clock scaling ("clk0" 2, "Enab"), the RPM ignores its
             * clock votes and brings the memory clock (BIMC) back from the
             * first deep sleep at 200 MHz instead of the boot chain's 800:
             * RAM then runs at a third of its speed. So PCNOC and SNOC
             * ("clk1" 0 and 1) at their highest level, the BIMC ("clk2" 0)
             * at 800 MHz or above (the RPM picks 1066), all in kHz, then
             * scaling on. The sleep set drops all three to 0 and enables
             * scaling there (protected_devices.c), so DDR still
             * self-refreshes. This is the one RPM channel, opened here. */
            {0x316b6c63,0,0x007a484b,0x7fffffff},{0x316b6c63,1,0x007a484b,0x7fffffff},
            {0x326b6c63,0,0x007a484b,800000},{0x306b6c63,2,0x62616e45,1},
            };
            if (b->rpm.pending) {
                result = rpm_poll(&b->rpm, now);
                if (result < 0) return failed(b, "RPM power/clock vote rejected");
                if (!result) break;
                ++b->vote;
                b->deadline = now + 5000000;
            }
            if (b->vote == sizeof(votes) / sizeof(votes[0])) {
                b->clock_scaling = true;
                advance(b, WB_CLOCKS);
                break;
            }
            if (!b->deadline) {
                /* Opening belongs to SMD; a late open acknowledgement must not
                 * consume the first vote's transmit-space budget. */
                result = smd_poll(&b->rpm_channel, now);
                if (result < 0) return failed(b, "RPM power/clock transport failed");
                if (!result) break;
                b->deadline = now + 5000000;
            }
            if (now >= b->deadline) break; /* Bound a vote waiting for transmit space. */
            const uint32_t *v = votes[b->vote];
            result = rpm_vote(&b->rpm, v[0], v[1], v[2], v[3], now);
            if (result < 0) return failed(b, "RPM power/clock transport failed");
            if (result == 1) b->deadline = 0; /* RPM owns the reply deadline after publication. */
            break;
        }
        case WB_CLOCKS:
            /* The stock WCNSS platform driver enables this shared QoS gate before
             * starting Pronto. It is a simple gate, with no branch-halt polling. */
            b->io.write(b->io.context, SNOC_QOSGEN, b->io.read(b->io.context, SNOC_QOSGEN) | 1);
            barrier();
            if (!(b->io.read(b->io.context, SNOC_QOSGEN) & 1))
                return failed(b, "WCNSS bus clock enable refused");
            /* PAS uses the crypto engine even though SOS2 does not access it.
             * Vote for the inherited 800 MHz GPLL0, never reprogram the shared PLL.
             * These APPS vote registers also serve touch/storage: preserve all bits. */
            b->io.write(b->io.context, PLL_VOTE, b->io.read(b->io.context, PLL_VOTE) | 1);
            barrier();
            if (!(b->io.read(b->io.context, PLL_VOTE) & 1))
                return failed(b, "WCNSS crypto PLL vote refused");
            b->state = WB_CLOCK_PLL;
            b->deadline = now + 1000000;
            break;
        case WB_CLOCK_PLL: {
            if (!(b->io.read(b->io.context, GPLL0_STATUS) & (1u << 17))) break;
            uint32_t cmd = b->io.read(b->io.context, CRYPTO_CMD);
            if (cmd & 1) break; /* Do not overwrite a loader's pending update. */
            uint32_t cfg = b->io.read(b->io.context, CRYPTO_CFG), rate = cfg & CRYPTO_CFG_MASK;
            /* Keep a usable inherited 80/100/160 MHz rate. Otherwise use the
             * board's 80 MHz (GPLL0 / 10), but never retune an active crypto core. */
            if (rate != 0x113 && rate != 0x10f && rate != 0x109) {
                if (clock_running(b, CRYPTO_CORE))
                    return failed(b, "WCNSS active crypto clock unsupported");
                b->io.write(b->io.context, CRYPTO_CFG, (cfg & ~CRYPTO_CFG_MASK) | 0x113);
                barrier();
                b->io.write(b->io.context, CRYPTO_CMD, cmd | 1);
                barrier();
            }
            b->state = WB_CLOCK_UPDATE;
            b->deadline = now + 1000000;
            break;
        }
        case WB_CLOCK_UPDATE: {
            if (b->io.read(b->io.context, CRYPTO_CMD) & 1) break;
            uint32_t rate = b->io.read(b->io.context, CRYPTO_CFG) & CRYPTO_CFG_MASK;
            if (rate != 0x113 && rate != 0x10f && rate != 0x109)
                return failed(b, "WCNSS crypto clock configuration refused");
            b->io.write(b->io.context, BRANCH_VOTE, b->io.read(b->io.context, BRANCH_VOTE) | 7);
            barrier();
            if ((b->io.read(b->io.context, BRANCH_VOTE) & 7) != 7)
                return failed(b, "WCNSS crypto bus clock votes refused");
            b->state = WB_CLOCK_READY;
            b->deadline = now + 1000000;
            break;
        }
        case WB_CLOCK_READY:
            if (!clock_running(b, CRYPTO_CORE) || !clock_running(b, CRYPTO_AXI) ||
                !clock_running(b, CRYPTO_AHB))
                break;
            if (!(b->io.read(b->io.context, GPLL0_STATUS) & (1u << 17)))
                return failed(b, "WCNSS crypto PLL lost lock");
            advance(b, WB_METADATA);
            break;
        case WB_METADATA:
            if (!qcom_scm_pas_init(b->metadata_physical))
                return failed(b, scm_error("WCNSS metadata rejected"));
            b->pas_initialized = true;
            advance(b, WB_MEMORY);
            break;
        case WB_MEMORY:
            if (!qcom_scm_pas_memory(b->physical, b->plan.span))
                return failed(b, scm_error("WCNSS relocation rejected"));
            advance(b, WB_COPY);
            break;
        case WB_COPY: {
            unsigned count = b->plan.span - b->copied;
            if (count > 65536) count = 65536;
            memset(b->memory + b->copied, 0, count);
            for (unsigned i = 0; i < b->plan.count; ++i) {
                const struct qcom_mdt_segment *s = &b->plan.segments[i];
                unsigned start = s->offset > b->copied ? s->offset : b->copied,
                         end = s->offset + s->size;
                if (end > b->copied + count) end = b->copied + count;
                if (start < end)
                    memcpy(b->memory + start, s->data + start - s->offset, end - start);
            }
            /* Local copying always advances and is bounded by the validated image
             * span and 64 KiB per poll; time spent servicing the UI is not a fault. */
            b->copied += count;
            if (b->copied == b->plan.span) advance(b, WB_SMP2P);
            break;
        }
        case WB_SMP2P:
            result = smem_allocate_peer(b->smem, SMEM_HOST_WCNSS, SMP2P_APPS_TO_WCNSS, SMP2P_BYTES,
                                        &b->smp_out);
            if (result < 0) return failed(b, "WCNSS notification allocation failed");
            if (!result) break;
            for (unsigned i = 0; i < SMP2P_BYTES; ++i) b->smp_out.data[i] = 0;
            shared_put(b->smp_out.data, 0x504d5324);
            shared_put(b->smp_out.data + 8, 4u << 16);
            shared_put(b->smp_out.data + 12, (1u << 16) | 16);
            for (unsigned i = 0; i < 13; ++i)
                b->smp_out.data[20 + i] = (unsigned char)"master-kernel"[i];
            shared_put(b->smp_out.data + 4, 0x101);
            b->io.signal(b->io.context, 1u << 18);
            /* MSM8916 WCSS five-wire pins: function 1, 6 mA, pull-up. */
            for (unsigned pin = 40; pin <= 44; ++pin)
                b->io.write(b->io.context, 0x01000000 + pin * 0x1000, (2u << 6) | (1u << 2) | 3);
            b->io.write(b->io.context, SPARE, b->io.read(b->io.context, SPARE) | (1u << 25));
            b->io.write(b->io.context, PMU, 0);
            b->io.write(b->io.context, PMU, IRIS_BASE);
            b->io.write(b->io.context, PMU, IRIS_BASE | IRIS_RESET);
            b->state = WB_IRIS_RESET;
            b->deadline = now + 100000;
            break;
        case WB_IRIS_RESET:
        case WB_IRIS_CONFIG: {
            uint32_t pmu = b->io.read(b->io.context, PMU);
            if (pmu & IRIS_RESET_BUSY) {
                if (now >= b->deadline) return value_timeout(b, " PMU=", pmu);
                break;
            }
            b->io.write(b->io.context, PMU, IRIS_BASE);
            if (b->state == WB_IRIS_RESET) {
                /* Preserve the read-port's low control bits, as the vendor does;
                 * bits 31:16 are the returned chip ID, not a command to replay. */
                uint32_t select = b->io.read(b->io.context, IRIS_READ) & 0xffff;
                b->io.write(b->io.context, IRIS_READ, select | 4);
                b->io.write(b->io.context, PMU, IRIS_BASE | IRIS_READ_START);
                ++b->iris_attempts;
                b->state = WB_IRIS_READ;
            } else {
                b->io.write(b->io.context, PMU, IRIS_BASE | IRIS_XO_START);
                b->state = WB_IRIS_XO;
            }
            b->deadline = now + 100000;
            break;
        }
        case WB_IRIS_READ: {
            uint32_t pmu = b->io.read(b->io.context, PMU);
            if (pmu & IRIS_READ_BUSY) {
                if (now >= b->deadline) return value_timeout(b, " PMU=", pmu);
                break;
            }
            b->iris_id = b->io.read(b->io.context, IRIS_READ);
            b->io.write(b->io.context, PMU, IRIS_BASE); /* Drop READ before resetting. */
            unsigned chip = b->iris_id >> 16;
            bool supported = chip == 0x5111 || chip == 0x5112; /* WCN3620 / WCN3620A, 19.2 MHz */
            if (!supported && b->iris_attempts == IRIS_ATTEMPTS) {
                const char prefix[] = "WCN3620 not detected: IRIS=";
                unsigned n = sizeof(prefix) - 1;
                memcpy(b->diagnostic, prefix, n);
                for (unsigned i = 0; i < 8; ++i)
                    b->diagnostic[n++] = "0123456789abcdef"[(b->iris_id >> (28 - 4 * i)) & 15];
                b->diagnostic[n] = 0;
                return failed(b, b->diagnostic);
            }
            /* Never start firmware with an unverified or different RF chip. The
             * second reset also follows stock's successful identification path. */
            b->io.write(b->io.context, PMU, IRIS_BASE | IRIS_RESET);
            b->state = supported ? WB_IRIS_CONFIG : WB_IRIS_RESET;
            b->deadline = now + 100000;
            break;
        }
        case WB_IRIS_XO: {
            uint32_t pmu = b->io.read(b->io.context, PMU);
            if (pmu & IRIS_XO_BUSY) {
                if (now >= b->deadline) return value_timeout(b, " PMU=", pmu);
                break;
            }
            b->io.write(b->io.context, PMU, 0x10);
            b->state = WB_SETTLE;
            b->deadline = now + 20000;
            break;
        }
        case WB_SETTLE:
            if (now < b->deadline) break; /* Minimum settling time, not a completion deadline. */
            advance(b, WB_AUTH);
            break;
        case WB_AUTH:
            b->crash_baseline = crash_reason(b, b->prior_crash);
            b->watchdog_clear_seen = !(b->io.read(b->io.context, WDOG_PENDING) & WDOG_BIT);
            barrier();
            if (!qcom_scm_pas_start())
                return failed(b, scm_error("WCNSS authentication/start failed"));
            b->authenticated = true;
            crypto_clocks(b, false);
            advance(b, WB_READY_WAIT);
            break;
        case WB_READY_WAIT: {
            /* Reuse the result observed above. A second lock attempt can be busy
             * and must not discard an already-observed ready acknowledgement. */
            if (notification == NOTIFY_VALID && (notification_value & 2)) advance(b, WB_CONTROL);
            else if (now >= b->deadline) {
                if (notification == NOTIFY_BUSY)
                    return failed(b, "WCNSS ready timeout: shared-memory lock busy");
                if (notification == NOTIFY_ABSENT) return absent_timeout(b);
                if (notification == NOTIFY_UNPUBLISHED)
                    return radio_failed(b, "WCNSS ready timeout: header unpublished");
                if (notification == NOTIFY_NEGOTIATING)
                    return radio_failed(b, "WCNSS ready timeout: negotiation incomplete");
                if (notification == NOTIFY_NO_ENTRY)
                    return radio_failed(b, "WCNSS ready timeout: slave-kernel missing");
                return value_timeout(b, " state=", notification_value);
            }
            break;
        }
        case WB_CONTROL:
            if (!b->control_open) {
                if (!open_channel(b, &b->control_channel, 6, "WCNSS_CTRL", wcn_signal, now,
                                  &result)) {
                    if (result < 0)
                        return failed(b, b->diagnostic[0] ? b->diagnostic
                                                          : "WCNSS control channel invalid");
                    break;
                }
                b->control_open = true;
                b->deadline = 0;
                if (!wcnss_control_start(&b->control, &b->control_channel, b->nv.data,
                                         (unsigned)b->nv.size, now))
                    return failed(b, "WCNSS board data invalid");
            }
            switch (wcnss_control_poll(&b->control, now)) {
                case WCNSS_FAILED: return failed(b, b->control.error);
                case WCNSS_READY: advance(b, WB_RUNNING); break;
                default: break;
            }
            break;
        case WB_RUNNING:
            if (wcnss_control_poll(&b->control, now) != WCNSS_READY)
                return failed(b, "WCNSS control link lost");
            break;
        default: return failed(b, "invalid wireless startup state");
    }
    /* Read acknowledgements/publications first, including on a late poll.
     * Only an operation that is still pending can time out. Calibration owns
     * its per-request budgets; no outer timer can discard its completed reply. */
    if (b->state == before && b->deadline && now >= b->deadline)
        return failed(b, timeout_error(b->state));
    return b->state;
}
