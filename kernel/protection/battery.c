#include "battery.h"
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_battery.h"
#endif
/* Every 30 s awake, every 5 s in standby (there as the chip wakes, every
 * 10 s). The gauge's current follows a change slowly: just after the screen
 * goes off, or a charger is pulled, it still reads the awake or charging
 * current, enough to swing a short standby's average, even below zero. So a
 * standby's average leaves out its first minute, as the standby trial's
 * phases did, and the minute after the gauge last reported charging; a
 * charger, hundreds of mA against standby's few, is named, not averaged. */
enum { AWAKE_US = 30000000, STANDBY_US = 5000000, SETTLE_US = 60000000 };
static struct battery_view view;
static struct battery_standby last;
static uint64_t next_poll, standby_since, settled_at;
static bool standby, standby_charger;
static int64_t standby_sum;
static unsigned standby_samples, standby_percent;

void battery_init(void) {
    view = (struct battery_view){0};
    last = (struct battery_standby){0};
    next_poll = 0;
    standby = false;
}

bool battery_supported(void) {
#ifdef SOS2_BOARD_REDMI2
    return true;
#else
    return false; /* QEMU virt has no battery; never invent a percentage. */
#endif
}

const struct battery_view *battery_view(void) { return &view; }

const struct battery_standby *battery_last_standby(void) { return &last; }

void battery_standby(bool active) {
    if (active == standby || !battery_supported()) return;
    standby = active;
    uint64_t now = time_us();
    if (active) {
        standby_since = now;
        settled_at = now + SETTLE_US;
        standby_sum = 0;
        standby_samples = 0;
        standby_charger = false;
        standby_percent = view.available ? view.percent : 0;
        next_poll = now + STANDBY_US;
        return;
    }
    next_poll = now; /* A fresh reading for the desktop. */
    if (!standby_samples && !(standby_charger && now - standby_since >= SETTLE_US)) return;
    unsigned used =
        view.available && standby_percent > view.percent ? standby_percent - view.percent : 0;
    /* Below 0 off charger is only the gauge's lag or offset. */
    int64_t average = standby_samples ? standby_sum / standby_samples : 0;
    last = (struct battery_standby){standby_samples, (unsigned)((now - standby_since) / 60000000),
                                    used, average > 0 ? (int)average : 0, standby_charger};
}

bool battery_pending(void) { return battery_supported() && time_us() >= next_poll; }

void battery_service(void *unused) {
    (void)unused;
#ifdef SOS2_BOARD_REDMI2
    struct battery_view sample = {0};
    if (redmi2_battery_read(&sample)) view = sample;
    else view = (struct battery_view){0};
#endif
    uint64_t now = time_us();
    if (standby && view.charging) {
        standby_charger = true;
        settled_at = now + SETTLE_US;
    } else if (standby && view.current && now >= settled_at) {
        standby_sum += view.milliamps;
        ++standby_samples;
    }
    next_poll = now + (standby ? STANDBY_US : AWAKE_US);
}
