#ifndef SOS2_BATTERY_H
#define SOS2_BATTERY_H
#include "platform.h"

/* The gauge's last reading. With current, milliamps is the current drawn from
 * the battery: negative while it charges. */
struct battery_view {
    bool available, charging, current;
    unsigned percent;
    int milliamps;
};

/* The last standby that lasted over a minute, sampled every 5 s while the
 * screen was off: its length, the charge it used and, over its samples off
 * charger, its average current, at least 0. A sample counts from a minute
 * after the screen went off or the gauge last reported charging, while the
 * gauge settles. charger: the gauge reported charging during it. samples is
 * 0 and charger false before the first such standby. */
struct battery_standby {
    unsigned samples, minutes, percent_used;
    int milliamps;
    bool charger;
};

void battery_init(void);
/* Whether this board has a battery gauge (the phone; QEMU has none). */
bool battery_supported(void);
const struct battery_view *battery_view(void);
const struct battery_standby *battery_last_standby(void);
/* Entering standby samples more often; leaving it publishes the average. */
void battery_standby(bool active);
bool battery_pending(void);
void battery_service(void *);
#endif
