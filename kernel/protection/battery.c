#include "battery.h"
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_battery.h"
#endif
/* The battery icon's level and charger, read every 30 s with the screen on,
 * at once when it comes on, and never while it is off, when nothing shows
 * them. A reading that fails leaves the icon as it was, until three in a
 * row have.
 * The level is the gauge's, steadied so that the icon does not jump: off a
 * charger it only falls and on one it only rises, and a step of 5% or more
 * is taken only once three readings in a row span at most two percentage
 * points and a minute or more from first to last; smaller steps in that
 * direction are taken at once, even after a brief wake. After standby or
 * three failed reads, the same confirmation permits either direction: a
 * charger may have come and gone while not polling.
 * The gauge is never reset: a reset estimates the charge afresh from the
 * voltage of the moment, which a load lowers, and on the handset the level
 * then jumped between half and near empty. */
enum { POLL_US = 30000000 };
static struct battery_view view;
static uint64_t next_poll;
static bool standby;
#ifdef SOS2_BOARD_REDMI2
enum { STEP = 5, AGREEING = 3, AGREE_SPREAD = 2, AGREE_US = 60000000, FAILURES = 3 };

static uint64_t step_since;
static unsigned shown, step_readings, step_low, step_high, failures;
static bool shown_valid, resync, step_charger;

/* The level to show for the gauge's reading. */
static unsigned steady(unsigned percent, bool charger, uint64_t now) {
    if (!shown_valid) {
        shown_valid = true;
        resync = false;
        return percent;
    }
    bool toward = charger ? percent > shown : percent < shown;
    unsigned step = !toward ? 0 : charger ? percent - shown : shown - percent;
    /* A short wake still updates small steps in the expected direction.
     * Keep resync pending for any later correction needing confirmation. */
    if (step < STEP && (toward || !resync)) {
        step_readings = 0;
        return toward ? percent : shown;
    }
    unsigned low = step_readings && step_low < percent ? step_low : percent,
             high = step_readings && step_high > percent ? step_high : percent;
    if (!step_readings || charger != step_charger || high - low > AGREE_SPREAD) {
        step_since = now;
        step_readings = 1;
        step_low = step_high = percent;
        step_charger = charger;
    } else {
        step_low = low;
        step_high = high;
        if (step_readings < AGREEING) ++step_readings;
    }
    if (step_readings < AGREEING || now - step_since < AGREE_US) return shown;
    step_readings = 0;
    resync = false;
    return percent;
}
#endif

void battery_init(void) {
    view = (struct battery_view){0};
    next_poll = 0;
    standby = false;
#ifdef SOS2_BOARD_REDMI2
    shown_valid = resync = false;
    step_readings = failures = 0;
#endif
}

bool battery_supported(void) {
#ifdef SOS2_BOARD_REDMI2
    return true;
#else
    return false; /* QEMU virt has no battery; never invent a percentage. */
#endif
}

const struct battery_view *battery_view(void) { return &view; }

void battery_standby(bool active) {
    if (active == standby || !battery_supported()) return;
    standby = active;
    next_poll = active ? UINT64_MAX : time_us();
#ifdef SOS2_BOARD_REDMI2
    step_readings = 0;
    if (!active) resync = true;
#endif
}

bool battery_pending(void) { return battery_supported() && time_us() >= next_poll; }

void battery_service(void *unused) {
    (void)unused;
    if (standby) return;
    uint64_t now = time_us();
    next_poll = now + POLL_US;
#ifdef SOS2_BOARD_REDMI2
    struct battery_view sample;
    if (!redmi2_battery_read(&sample)) {
        step_readings = 0;
        if (failures < FAILURES) ++failures;
        if (failures == FAILURES) {
            view.available = false;
            resync = true;
        }
        return;
    }
    failures = 0;
    shown = steady(sample.percent, sample.charger, now);
    view = sample;
    view.percent = shown;
#endif
}
