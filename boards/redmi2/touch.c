#include "redmi2_touch.h"
#include "redmi2_touch_hw.h"
#include "qup_i2c.h"

/* Independently implemented from these protocol/hardware descriptions:
 * Linux msm8916-wingtech-wt88047.dts: edt,edt-ft5506, address 0x38, 720x1280.
 * Linux edt-ft5x06.c: standard 3-byte header + 6-byte records, ten contacts for
 * ft5506, event/id/coordinate fields and M06/EV identification exceptions.
 * FocalTech ft5x06.c: TD_STATUS lower nibble and ID 0xf end marker.
 * https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/msm8916-wingtech-wt88047.dts
 * https://github.com/torvalds/linux/blob/master/drivers/input/touchscreen/edt-ft5x06.c
 * https://github.com/focaltech-systems/drivers-input-touchscreen-FTS_driver/blob/master/ft5x06.c
 * The capacitive keys below the display are contacts on row 1344, below the
 * 1280 display rows; the handset's own kernel passes them on as touches and
 * Android maps them to BACK, APP_SWITCH and HOME (ft5x06_ts.c, ft5x06_ts.kl).
 * Left to right the keys are Menu, Home and Back (handset reviews).
 * https://github.com/LineageOS/android_kernel_wingtech_msm8916/blob/lineage-17.1/drivers/input/touchscreen/ft5x06_ts.c
 * https://github.com/LineageOS/android_device_wingtech_wt88047/blob/lineage-17.1/keylayout/ft5x06_ts.kl
 * https://www.androidheadlines.com/2015/02/featured-review-xiaomi-redmi-2-prince-pauper.html
 *
 * This is a single-pointer consumer, not a multitouch gesture engine; of the
 * keys, Home and Back are reported: as a finger lands on one, for feedback
 * (redmi2_touch_key_pressed), and as a tap (redmi2_touch_key_tapped). No
 * source gives the keys' contact positions; Back is the right third of the
 * row, as taps on the handset confirmed, so Home is taken as the middle one.
 * Firmware model IDs are unreliable; successful reads establish a compatible
 * report layout, not an exact IC identity. M06 and the axis-swapped EV layout
 * are not silently treated as standard FT data. The only controller register
 * write is standby's hibernate request.
 */

#include "platform.h"
#define now_us time_us

enum {
    TOUCH_ADDRESS = 0x38,
    CONTACTS = 10,
    REPORT_BYTES = 3 + CONTACTS * 6,
    /* Idle and one-finger reports end within two records: read those first. */
    PREFIX_RECORDS = 2,
    PREFIX_BYTES = 3 + PREFIX_RECORDS * 6,
    SENSOR_WIDTH = 720,
    SENSOR_HEIGHT = 1280,
    /* Three keys share the width below the display: Menu, Home and Back. */
    HOME_KEY_LEFT = SENSOR_WIDTH / 3,
    BACK_KEY_LEFT = SENSOR_WIDTH * 2 / 3,
    SAMPLE_INTERVAL_US = 8000,
    IDLE_INTERVAL_US = 50000,
    LOST_REPORT_US = 250000,
    MAX_FAILURES = 3,
    /* FocalTech power mode register: 3 hibernates; a reset pulse wakes it. */
    PMODE_REGISTER = 0xa5,
    PMODE_HIBERNATE = 3,
    RESET_US = 5000,
    START_US = 300000,
    NO_CONTACT = 0xff,
};

/* BLSP1 QUP5 on GPIO18/19 at 100 kHz; touch_hw.c enables its core and AHB
 * clocks, the pins' mux and the controller's supplies before init. */
static struct qup_i2c touch_bus = {.base = 0x078b9000};

struct touch_report {
    uint16_t active, off_panel;
    bool no_events;                    /* Padding carries no contact state, including no release. */
    unsigned count;                    /* Active contacts on the display. */
    uint16_t x[CONTACTS], y[CONTACTS]; /* On and off the display. */
};

static bool available, wait_for_all_up, down;
static bool suspended, hibernating;
static uint64_t start_after; /* After waking from hibernation: no reads before. */
static uint8_t tracked_id;
/* A contact that began below the display, the key it began on if it has
 * stayed there with nothing on the display, and its start and a finished tap
 * not yet taken. */
static uint8_t key_id;
static enum redmi2_touch_key key, key_pressed, key_tapped;
static uint16_t present; /* Contacts in the last valid report. */
static unsigned failures;
static uint32_t display_width, display_height, pointer_x, pointer_y;
static uint64_t last_read, last_good_report;

static bool ep0_prefix(const uint8_t *bytes) {
    return (bytes[0] == 'E' || bytes[0] == 'e') && (bytes[1] == 'P' || bytes[1] == 'p') &&
           bytes[2] == '0';
}

static bool empty_record(const uint8_t *record) {
    for (unsigned i = 0; i < 6; ++i)
        if (record[i]) return false;
    return true;
}

static bool padding_report(const uint8_t *raw, unsigned slots) {
    if (raw[0]) return false;
    for (unsigned i = 1; i < 3 + slots * 6; ++i)
        if (raw[i] != 0xff) return false;
    return true;
}

enum { DECODE_BAD, DECODE_OK, DECODE_SHORT };

/* Decode a report of which the first `slots` records were read. DECODE_SHORT
 * means those records cannot settle it, and the complete report is needed. */
static int decode_records(const uint8_t *raw, unsigned slots, struct touch_report *report) {
    *report = (struct touch_report){0};
    /* Device-mode bits 6:4 select normal/factory operation. */
    if (raw[0] & 0x70) return DECODE_BAD; /* controller not in normal mode */
    /* All-padding form matching the observed WT88047 startup prefix. Linux
     * skips reserved/ID15 records and
     * preserves existing contact slots; it does not turn them into releases.
     * Require the entire report, not just its prefix. */
    if (padding_report(raw, slots)) {
        if (slots < CONTACTS) return DECODE_SHORT;
        report->no_events = true;
        return DECODE_OK;
    }
    unsigned expected = raw[2] & 0x0f;
    if (expected > CONTACTS) return DECODE_BAD; /* contact count exceeds ten */
    uint16_t seen = 0;
    unsigned declared = 0; /* Active contacts on or off the display. */
    for (unsigned slot = 0; slot < CONTACTS; ++slot) {
        if (slot == slots) return DECODE_SHORT;
        const uint8_t *record = raw + 3 + slot * 6;
        unsigned type = record[0] >> 6, id = record[2] >> 4;
        if (id == 0x0f) break;
        /* Some firmware zero-fills unused slots. A zero DOWN/id0 is only
         * padding once all declared contacts have already been accounted for. */
        if (declared == expected && empty_record(record)) break;
        /* A reserved event cannot establish an all-up transition. Treat it as
         * uncertain data so that a malformed release cannot become a click. */
        if (type == 3) return DECODE_BAD;         /* reserved contact event */
        if (id >= CONTACTS) return DECODE_BAD;    /* contact ID exceeds nine */
        if (seen & (1u << id)) return DECODE_BAD; /* duplicate contact ID */
        seen |= (uint16_t)(1u << id);
        if (type == 1) continue; /* UP coordinates are unspecified; retain last position. */
        uint16_t x = (uint16_t)(((record[0] & 0x0f) << 8) | record[1]);
        uint16_t y = (uint16_t)(((record[2] & 0x0f) << 8) | record[3]);
        ++declared;
        report->x[id] = x;
        report->y[id] = y;
        /* Contacts beyond the display, such as the capacitive keys below it, are
         * valid reports but never pointer contacts. Rejecting them as malformed
         * would disable touch after a key is held for three samples. */
        if (x >= SENSOR_WIDTH || y >= SENSOR_HEIGHT) {
            report->off_panel |= (uint16_t)(1u << id);
            continue;
        }
        report->active |= (uint16_t)(1u << id);
        ++report->count;
    }
    return declared == expected ? DECODE_OK : DECODE_BAD;
}

static bool decode_report(const uint8_t *raw, struct touch_report *report) {
    return decode_records(raw, CONTACTS, report) == DECODE_OK;
}

static uint32_t scale_coordinate(uint16_t value, uint32_t extent, uint32_t sensor_extent) {
    return (uint32_t)((uint64_t)value * (extent - 1) / (sensor_extent - 1));
}

static enum redmi2_touch_key key_at(uint16_t x, uint16_t y) {
    if (y < SENSOR_HEIGHT || x >= SENSOR_WIDTH) return REDMI2_TOUCH_NO_KEY;
    return x >= BACK_KEY_LEFT   ? REDMI2_TOUCH_BACK
           : x >= HOME_KEY_LEFT ? REDMI2_TOUCH_HOME
                                : REDMI2_TOUCH_NO_KEY; /* Menu: unused */
}

/* A finger on the display or a key: reports must keep arriving. */
static bool holding(void) { return down || key_id != NO_CONTACT; }

static bool cancel(struct pointer_event *event) {
    bool had_pointer = down;
    down = false;
    tracked_id = NO_CONTACT;
    key_id = NO_CONTACT;
    key = REDMI2_TOUCH_NO_KEY;
    wait_for_all_up = true;
    if (had_pointer) {
        *event = (struct pointer_event){
            .x = pointer_x, .y = pointer_y, .down = false, .cancelled = true};
        return true;
    }
    return false;
}

/* A lost or invalid report cancels the gesture; three in a row disable touch. */
static bool bad_report(struct pointer_event *event) {
    if (++failures >= MAX_FAILURES) available = false;
    return cancel(event);
}

bool redmi2_touch_init(uint32_t width, uint32_t height) {
    available = false;
    down = false;
    wait_for_all_up = true;
    failures = 0;
    suspended = hibernating = false;
    start_after = 0;
    key_id = NO_CONTACT;
    key = key_pressed = key_tapped = REDMI2_TOUCH_NO_KEY;
    present = 0;
    tracked_id = NO_CONTACT;
    pointer_x = pointer_y = 0;
    display_width = width;
    display_height = height;
    if (!width || !height || width > 8192 || height > 8192) return false;
    if (!redmi2_touch_hw_init() || !qup_i2c_init(&touch_bus)) return false;
    uint8_t name[22], identity[3] = {0};
    if (!qup_i2c_read(&touch_bus, TOUCH_ADDRESS, 0xbb, name, sizeof(name))) return false;
    if (ep0_prefix(name + 1)) return false; /* EDT M06 report protocol: unsupported */
    if (!ep0_prefix(name)) {
        if (!qup_i2c_read(&touch_bus, TOUCH_ADDRESS, 0xa6, identity, 2) ||
            !qup_i2c_read(&touch_bus, TOUCH_ADDRESS, 0xa8, identity + 2, 1))
            return false;
        /* Linux's Evervision FT5726 variant has reversed coordinate order. */
        if (identity[2] == 0x59) return false;
    }
    /* Match Linux's post-identification dummy register-0 read. EP0700MLP1 is
     * documented to return bogus data on its first register access. Its value
     * and a failed transfer are discarded; the full report below must pass. */
    uint8_t dummy = 0;
    (void)qup_i2c_read(&touch_bus, TOUCH_ADDRESS, 0, &dummy, 1);
    uint8_t bytes[REPORT_BYTES];
    struct touch_report report;
    if (!qup_i2c_read(&touch_bus, TOUCH_ADDRESS, 0x00, bytes, sizeof(bytes)) ||
        !decode_report(bytes, &report))
        return false;
    last_read = last_good_report = now_us();
    present = (uint16_t)(report.active | report.off_panel);
    wait_for_all_up = report.no_events || present;
    available = true;
    return true;
}

enum redmi2_touch_key redmi2_touch_key_pressed(void) {
    enum redmi2_touch_key pressed = key_pressed;
    key_pressed = REDMI2_TOUCH_NO_KEY;
    return pressed;
}

enum redmi2_touch_key redmi2_touch_key_tapped(void) {
    enum redmi2_touch_key tapped = key_tapped;
    key_tapped = REDMI2_TOUCH_NO_KEY;
    return tapped;
}

void redmi2_touch_cancel(void) {
    down = false;
    tracked_id = NO_CONTACT;
    key_id = NO_CONTACT;
    key = key_pressed = key_tapped = REDMI2_TOUCH_NO_KEY;
    wait_for_all_up = true;
}

void redmi2_touch_suspend(void) {
    if (suspended) return;
    suspended = true;
    redmi2_touch_cancel();
    /* A scanning controller draws power all standby. Hibernate it, as Linux's
     * edt-ft5x06 suspend does. Like Linux, wake resets it even if the request
     * reported a failure: the bytes may have reached the controller before
     * the error. */
    hibernating = available;
    if (!available) return;
    (void)qup_i2c_write(&touch_bus, TOUCH_ADDRESS, PMODE_REGISTER, PMODE_HIBERNATE);
    /* Then held in reset until waking, whose reset pulse ends it, as Linux's
     * power-off suspend asserts reset before cutting the supply: hibernated
     * but out of reset it still drew about 1 mA. Its supply (L17, shared with
     * the panel and motion sensor) goes off while the chip sleeps (rails.c).
     * Its interrupt line is pulled down, as the vendor's suspend pins leave
     * it: the controller no longer holds it up. */
    (void)redmi2_touch_hw_reset(true);
    (void)redmi2_touch_hw_interrupt_pull(false);
}

void redmi2_touch_resume(void) {
    if (!suspended) return;
    if (available) (void)redmi2_touch_hw_interrupt_pull(true);
    if (hibernating) {
        /* edt-ft5x06 resume: reset held 5 ms leaves hibernation, then 300 ms
         * for the controller to start before it is read again. */
        hibernating = false;
        uint64_t start = now_us();
        bool asserted = redmi2_touch_hw_reset(true);
        while (asserted && now_us() - start < RESET_US) { }
        if (!asserted || !redmi2_touch_hw_reset(false)) available = false;
        start_after = now_us() + START_US;
    }
    /* Off-screen time cannot extend a gesture or count as a report timeout.
     * Polling begins with a fresh interval, and only actual all-up data may
     * end the release wait. Preserve any existing failure state. */
    if (available) last_read = last_good_report = start_after > now_us() ? start_after : now_us();
    suspended = false;
}

bool redmi2_touch_poll(struct pointer_event *event) {
    if (suspended || !available || !event) return false;
    uint64_t timestamp = now_us();
    if (timestamp < start_after) return false;
    if (holding() && timestamp - last_good_report > LOST_REPORT_US) return cancel(event);
    uint64_t elapsed = timestamp - last_read;
    if (elapsed < SAMPLE_INTERVAL_US) return false;
    if (!holding() && !wait_for_all_up && !redmi2_touch_irq_pending() && elapsed < IDLE_INTERVAL_US)
        return false;

    uint8_t bytes[REPORT_BYTES];
    struct touch_report report;
    /* A 100 kHz read of the whole report busy-waits CPU 0 for about 6 ms.
     * Most reports end within the first two records; only when those cannot
     * settle a report, read the complete one again in a single transaction,
     * so records from two controller frames are never combined. */
    bool read_ok = qup_i2c_read(&touch_bus, TOUCH_ADDRESS, 0x00, bytes, PREFIX_BYTES);
    int decoded = read_ok ? decode_records(bytes, PREFIX_RECORDS, &report) : DECODE_BAD;
    if (decoded == DECODE_SHORT) {
        read_ok = qup_i2c_read(&touch_bus, TOUCH_ADDRESS, 0x00, bytes, sizeof(bytes));
        decoded = read_ok ? decode_records(bytes, CONTACTS, &report) : DECODE_BAD;
    }
    /* Rate-limit from completion, not the start of a possibly slow transaction. */
    last_read = now_us();
    if (decoded != DECODE_OK) return bad_report(event);
    /* A read may begin within the gesture deadline and finish after it. Do
     * not let even valid all-up data turn that uncertain gap into a click.
     * Failed/malformed reads above retain their failure-counting behavior. */
    if (holding() && last_read - last_good_report > LOST_REPORT_US) return cancel(event);
    /* No contact update: do not release, synchronize, forgive earlier errors,
     * or extend the last valid report's deadline for an outstanding gesture. */
    if (report.no_events) return false;
    last_good_report = last_read;
    failures = 0;
    uint16_t contacts = (uint16_t)(report.active | report.off_panel);
    uint16_t appeared = (uint16_t)(contacts & ~present);
    present = contacts;
    if (wait_for_all_up) {
        if (!contacts) wait_for_all_up = false;
        return false;
    }
    if (key_id != NO_CONTACT) {
        uint16_t bit = (uint16_t)(1u << key_id);
        /* A finger that slides from the keys onto the display is neither. */
        if (report.active & bit) return cancel(event);
        if (!(report.off_panel & bit)) {
            if (key != REDMI2_TOUCH_NO_KEY && !report.count) key_tapped = key;
            key_id = NO_CONTACT;
            key = REDMI2_TOUCH_NO_KEY;
        } else if (report.count || key_at(report.x[key_id], report.y[key_id]) != key) {
            key = REDMI2_TOUCH_NO_KEY;
        }
    } else if (!down && !report.count) {
        /* A key press starts with a new contact and nothing on the display, so
         * a palm resting on the keys during a gesture never becomes one. */
        uint16_t keys = (uint16_t)(appeared & report.off_panel);
        for (uint8_t id = 0; keys && id < CONTACTS; ++id)
            if (keys & (1u << id)) {
                key_id = id;
                key = key_at(report.x[id], report.y[id]);
                if (key != REDMI2_TOUCH_NO_KEY) key_pressed = key;
                break;
            }
    }
    if (down) {
        /* A finger that slides off the display never completes a click. */
        if (report.off_panel & (1u << tracked_id)) return cancel(event);
        if (!report.count) {
            down = false;
            tracked_id = NO_CONTACT;
            *event = (struct pointer_event){
                .x = pointer_x, .y = pointer_y, .down = false, .cancelled = false};
            return true;
        }
        if (!(report.active & (1u << tracked_id))) return cancel(event);
    } else {
        if (!report.count) return false;
        for (tracked_id = 0; tracked_id < CONTACTS; ++tracked_id)
            if (report.active & (1u << tracked_id)) break;
    }
    uint32_t x = scale_coordinate(report.x[tracked_id], display_width, SENSOR_WIDTH);
    uint32_t y = scale_coordinate(report.y[tracked_id], display_height, SENSOR_HEIGHT);
    bool changed = !down || x != pointer_x || y != pointer_y;
    pointer_x = x;
    pointer_y = y;
    down = true;
    if (changed) *event = (struct pointer_event){.x = x, .y = y, .down = true, .cancelled = false};
    return changed;
}
