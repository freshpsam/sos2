#include "system_clock.h"
#include "fs.h"
#include "page_host.h"
#include "runtime_files.h"
#include "platform.h"

enum { RECORD_BYTES = 40 };

#define POLL_US         5000000ull
#define CHECKPOINT_US   300000000ull
#define MAX_AGE_SECONDS 315576000u

struct calibration {
    uint32_t reference, floor;
    uint64_t epoch_us;
    bool valid;
};
static struct system_clock_ops rtc;
static struct system_clock_view view;
static struct calibration calibration;
static uint64_t wall_base, monotonic_base, last_sample, next_poll, next_checkpoint;
static uint64_t requested_us, requested_at;
static uint32_t last_counter;
static bool tracking, have_record, working, requested;

static uint32_t get32(const unsigned char *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const unsigned char *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

static void put32(unsigned char *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (8 * i));
}

static uint32_t crc(const unsigned char *p, unsigned n) {
    uint32_t value = ~0u;
    while (n--) {
        value ^= *p++;
        for (unsigned i = 0; i < 8; ++i) value = (value >> 1) ^ (0xedb88320u & -(value & 1));
    }
    return ~value;
}

static bool in_range(uint64_t us) {
    return us >= SYSTEM_CLOCK_MIN_SECONDS * 1000000 && us < SYSTEM_CLOCK_END_SECONDS * 1000000;
}

static bool decode(struct calibration *out, const unsigned char *data, unsigned size) {
    if (size != RECORD_BYTES || memcmp(data, "SOTC", 4) || get32(data + 4) != 1 ||
        get32(data + 8) != (unsigned)rtc.kind || get32(data + 12) > 1 || get32(data + 32) ||
        get32(data + 36) != crc(data, 36))
        return false;
    *out = (struct calibration){get32(data + 16), get32(data + 20), get64(data + 24),
                                get32(data + 12) != 0};
    return out->floor >= out->reference && in_range(out->epoch_us);
}

static bool persist(const struct calibration *value) {
    unsigned char data[RECORD_BYTES] = {0};
    memcpy(data, "SOTC", 4);
    put32(data + 4, 1);
    put32(data + 8, rtc.kind);
    put32(data + 12, value->valid);
    put32(data + 16, value->reference);
    put32(data + 20, value->floor);
    put32(data + 24, (uint32_t)value->epoch_us);
    put32(data + 28, (uint32_t)(value->epoch_us >> 32));
    put32(data + 36, crc(data, 36));
    return !fs_settings_write(FS_SETTING_CLOCK, data, sizeof(data));
}

static bool restored_time(const struct calibration *value, uint32_t counter, uint64_t *us) {
    if (!value->valid || counter < value->floor || counter < value->reference ||
        counter - value->reference > MAX_AGE_SECONDS)
        return false;
    *us = value->epoch_us + (uint64_t)(counter - value->reference) * 1000000;
    return in_range(*us);
}

static bool bootstrap(struct calibration *out) {
    if (rtc.kind != ARM_RTC_PMIC) return false;
    const struct runtime_file *f = runtime_file_find("/system/rtc-anchor");
    if (!f || f->size != 24) return false;
    const unsigned char *p = runtime_file_data(f);
    uint64_t seconds = get64(p + 16);
    if (get32(p) != 0x41435452 || get32(p + 4) != 1 || get32(p + 12) ||
        seconds < SYSTEM_CLOCK_MIN_SECONDS || seconds >= SYSTEM_CLOCK_END_SECONDS)
        return false;
    *out = (struct calibration){get32(p + 8), get32(p + 8), seconds * 1000000, true};
    return true;
}

static void publish(void) { page_host_clock_status(view.ready); }

static void apply(uint64_t us, uint64_t sample, uint32_t counter, bool monitor) {
    wall_base = us;
    monotonic_base = last_sample = sample;
    last_counter = counter;
    next_poll = time_us() + POLL_US;
    next_checkpoint = time_us() + CHECKPOINT_US;
    tracking = monitor;
    view.ready = true;
}

static void invalidate(const char *message) {
    view.ready = view.saved = false;
    tracking = false;
    view.message = message;
    publish();
    /* Persist invalidation so neither a packaged anchor nor the RTC's assumed
     * Unix epoch silently restores time after a reset/frozen counter. */
    if (view.storage) {
        calibration.valid = false;
        if (persist(&calibration)) have_record = true;
        else view.message = "Clock invalid; correction could not be saved";
    }
}

void system_clock_init(const struct system_clock_ops *ops) {
    rtc = ops ? *ops : (struct system_clock_ops){0};
    view = (struct system_clock_view){.storage = fs_persistent_ready()};
    calibration = (struct calibration){0};
    tracking = have_record = working = requested = false;
    view.message = "Waiting for internet time";
    unsigned char data[RECORD_BYTES];
    int size = fs_settings_read(FS_SETTING_CLOCK, data, sizeof(data));
    bool absent = size == USER_ENOENT || size == USER_ENODEV;
    if (!absent) {
        if (size < 0 || !decode(&calibration, data, (unsigned)size)) {
            view.message = "Saved clock invalid; waiting for internet time";
            goto done;
        }
        have_record = true;
        if (!calibration.valid) {
            view.message = "Device clock needs correction";
            goto done;
        }
    }
    uint32_t counter = 0;
    if (!rtc.read || (rtc.kind != ARM_RTC_UNIX && rtc.kind != ARM_RTC_PMIC) ||
        !rtc.read(&counter)) {
        view.message = "Device clock unavailable; waiting for internet time";
        if (have_record) invalidate(view.message);
        goto done;
    }
    view.rtc_available = true;
    uint64_t sample = time_us(), us = 0;
    bool imported = false;
    if (!have_record) {
        if (rtc.kind == ARM_RTC_UNIX && in_range((uint64_t)counter * 1000000))
            calibration = (struct calibration){counter, counter, (uint64_t)counter * 1000000, true};
        else if (bootstrap(&calibration)) imported = true;
        else goto done;
    }
    if (!restored_time(&calibration, counter, &us)) {
        view.message = "Device clock reset or calibration expired";
        if (have_record) invalidate(view.message);
        goto done;
    }
    apply(us, sample, counter, true);
    view.saved = have_record;
    view.message = have_record ? "Date and time restored"
                   : imported  ? "Using packaged clock calibration"
                               : "Using device date and time";
    if (view.storage && (have_record || imported)) {
        struct calibration checkpoint = calibration;
        checkpoint.floor = counter;
        if (persist(&checkpoint)) {
            calibration = checkpoint;
            have_record = view.saved = true;
        } else view.message = "Time works; reboot checkpoint could not be saved";
    }
done:
    publish();
}

const struct system_clock_view *system_clock_view(void) { return &view; }

bool system_clock_read(uint64_t *us) {
    if (!us || !view.ready) return false;
    uint64_t now = time_us();
    if (now < monotonic_base ||
        now - monotonic_base >= SYSTEM_CLOCK_END_SECONDS * 1000000 - wall_base)
        return false;
    *us = wall_base + (now - monotonic_base);
    return true;
}

bool system_clock_synchronize(uint64_t us) {
    if (!in_range(us) || working || requested) return false;
    requested_us = us;
    requested_at = time_us();
    requested = true;
    view.busy = true;
    publish();
    return true;
}

void system_clock_network_status(enum clock_network_state state) {
    if (view.network != state) {
        view.network = state;
        publish();
    }
}

bool system_clock_pending(void) { return requested || (tracking && time_us() >= next_poll); }

void system_clock_service(void *unused) {
    (void)unused;
    if (!system_clock_pending()) return;
    struct system_clock_view before = view;
    working = true;
    view.storage = fs_persistent_ready();
    if (requested) {
        requested = false;
        uint32_t counter = 0;
        view.rtc_available = rtc.read && (rtc.kind == ARM_RTC_UNIX || rtc.kind == ARM_RTC_PMIC) &&
                             rtc.read(&counter);
        uint64_t sample = time_us(), us = requested_us + (sample - requested_at);
        if (sample < requested_at || !in_range(us))
            view.message = "Internet time outside supported range";
        else {
            /* A missing/failing disk or RTC must not prevent a RAM boot from
             * getting useful time. Keep all filesystem work on this worker. */
            calibration = (struct calibration){counter, counter, us, true};
            have_record = view.saved = view.storage && view.rtc_available && persist(&calibration);
            apply(us, sample, counter, view.rtc_available);
            view.message = view.saved ? "Internet UTC synchronized and saved"
                                      : "Internet UTC synchronized for this boot";
        }
    } else {
        uint32_t counter = 0;
        view.rtc_available = rtc.read && rtc.read(&counter);
        uint64_t sample = time_us();
        next_poll = sample + POLL_US;
        uint64_t elapsed = sample - last_sample,
                 advance =
                     counter >= last_counter ? (uint64_t)(counter - last_counter) * 1000000 : 0;
        uint64_t difference = elapsed > advance ? elapsed - advance : advance - elapsed, us;
        if (!view.rtc_available || counter < last_counter || sample < last_sample ||
            difference > 2000000 || !restored_time(&calibration, counter, &us))
            invalidate("Device clock stopped or changed; waiting for internet time");
        else {
            last_counter = counter;
            last_sample = sample;
            if (have_record && view.storage && sample >= next_checkpoint) {
                struct calibration checkpoint = calibration;
                checkpoint.floor = counter;
                if (persist(&checkpoint)) {
                    calibration = checkpoint;
                    next_checkpoint = sample + CHECKPOINT_US;
                } else {
                    next_checkpoint = sample + 60000000;
                    view.message = "Time works; reboot checkpoint could not be saved";
                }
            }
        }
    }
    working = false;
    view.busy = false;
    if (memcmp(&before, &view, sizeof(view))) publish();
}
