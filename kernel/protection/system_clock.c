#include "system_clock.h"
#include "fs.h"
#include "page_host.h"
#include "sha256.h"
#include "platform.h"
#include "wire.h"

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
static struct calibration calibration;
static uint64_t wall_base, monotonic_base, last_sample, next_poll, next_checkpoint;
static uint64_t requested_us, requested_at;
static uint32_t last_counter;
static bool ready, storage, tracking, have_record, working, requested;

static bool in_range(uint64_t us) {
    return us >= SYSTEM_CLOCK_MIN_SECONDS * 1000000 && us < SYSTEM_CLOCK_END_SECONDS * 1000000;
}

static bool decode(struct calibration *out, const unsigned char *data, unsigned size) {
    if (size != RECORD_BYTES || memcmp(data, "SOTC", 4) || le32(data + 4) != 1 ||
        le32(data + 8) != (unsigned)rtc.kind || le32(data + 12) > 1 || le32(data + 32) ||
        le32(data + 36) != crc32_ieee(data, 36))
        return false;
    *out = (struct calibration){le32(data + 16), le32(data + 20), le64(data + 24),
                                le32(data + 12) != 0};
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
    put32(data + 36, crc32_ieee(data, 36));
    return !fs_settings_write(FS_SETTING_CLOCK, data, sizeof(data));
}

static bool restored_time(const struct calibration *value, uint32_t counter, uint64_t *us) {
    if (!value->valid || counter < value->floor || counter < value->reference ||
        counter - value->reference > MAX_AGE_SECONDS)
        return false;
    *us = value->epoch_us + (uint64_t)(counter - value->reference) * 1000000;
    return in_range(*us);
}

/* The header's clock shows the time once it is known. */
static void publish(void) { page_host_clock_status(); }

static void apply(uint64_t us, uint64_t sample, uint32_t counter, bool monitor) {
    wall_base = us;
    monotonic_base = last_sample = sample;
    last_counter = counter;
    next_poll = time_us() + POLL_US;
    next_checkpoint = time_us() + CHECKPOINT_US;
    tracking = monitor;
    ready = true;
}

static void invalidate(void) {
    ready = tracking = false;
    publish();
    /* Persist invalidation so the RTC's assumed Unix epoch does not silently
     * restore time after a reset or frozen counter. */
    if (storage) {
        calibration.valid = false;
        if (persist(&calibration)) have_record = true;
    }
}

void system_clock_init(const struct system_clock_ops *ops) {
    rtc = ops ? *ops : (struct system_clock_ops){0};
    storage = fs_persistent_ready();
    calibration = (struct calibration){0};
    ready = tracking = have_record = working = requested = false;
    unsigned char data[RECORD_BYTES];
    int size = fs_settings_read(FS_SETTING_CLOCK, data, sizeof(data));
    bool absent = size == USER_ENOENT || size == USER_ENODEV;
    if (!absent) {
        if (size < 0 || !decode(&calibration, data, (unsigned)size)) goto done;
        have_record = true;
        if (!calibration.valid) goto done;
    }
    uint32_t counter = 0;
    if (!rtc.read || (rtc.kind != ARM_RTC_UNIX && rtc.kind != ARM_RTC_PMIC) ||
        !rtc.read(&counter)) {
        if (have_record) invalidate();
        goto done;
    }
    uint64_t sample = time_us(), us = 0;
    if (!have_record) {
        if (rtc.kind != ARM_RTC_UNIX || !in_range((uint64_t)counter * 1000000)) goto done;
        calibration = (struct calibration){counter, counter, (uint64_t)counter * 1000000, true};
    }
    if (!restored_time(&calibration, counter, &us)) {
        if (have_record) invalidate();
        goto done;
    }
    apply(us, sample, counter, true);
    if (storage && have_record) {
        struct calibration checkpoint = calibration;
        checkpoint.floor = counter;
        if (persist(&checkpoint)) calibration = checkpoint;
    }
done:
    publish();
}

bool system_clock_ready(void) { return ready; }

bool system_clock_read(uint64_t *us) {
    if (!us || !ready) return false;
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
    return true;
}

bool system_clock_pending(void) { return requested || (tracking && loop_time_us() >= next_poll); }

void system_clock_service(void *unused) {
    (void)unused;
    if (!system_clock_pending()) return;
    working = true;
    storage = fs_persistent_ready();
    if (requested) {
        requested = false;
        uint32_t counter = 0;
        bool rtc_available = rtc.read && (rtc.kind == ARM_RTC_UNIX || rtc.kind == ARM_RTC_PMIC) &&
                             rtc.read(&counter);
        uint64_t sample = time_us(), us = requested_us + (sample - requested_at);
        if (sample >= requested_at && in_range(us)) {
            /* A missing/failing disk or RTC must not prevent a RAM boot from
             * getting useful time. Keep all filesystem work on this worker. */
            calibration = (struct calibration){counter, counter, us, true};
            have_record = storage && rtc_available && persist(&calibration);
            apply(us, sample, counter, rtc_available);
            publish();
        }
    } else {
        uint32_t counter = 0;
        bool rtc_available = rtc.read && rtc.read(&counter);
        uint64_t sample = time_us();
        next_poll = sample + POLL_US;
        uint64_t elapsed = sample - last_sample,
                 advance =
                     counter >= last_counter ? (uint64_t)(counter - last_counter) * 1000000 : 0;
        uint64_t difference = elapsed > advance ? elapsed - advance : advance - elapsed, us;
        if (!rtc_available || counter < last_counter || sample < last_sample ||
            difference > 2000000 || !restored_time(&calibration, counter, &us))
            invalidate();
        else {
            last_counter = counter;
            last_sample = sample;
            if (have_record && storage && sample >= next_checkpoint) {
                struct calibration checkpoint = calibration;
                checkpoint.floor = counter;
                if (persist(&checkpoint)) {
                    calibration = checkpoint;
                    next_checkpoint = sample + CHECKPOINT_US;
                } else next_checkpoint = sample + 60000000;
            }
        }
    }
    working = false;
}
