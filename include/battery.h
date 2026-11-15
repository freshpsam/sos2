#ifndef SOS2_BATTERY_H
#define SOS2_BATTERY_H
#include "platform.h"

/* What the battery icon shows. charger: a charger is connected; charging: it
 * charges the battery; percent: the gauge's level, steadied (battery.c). */
struct battery_view {
    bool available, charger, charging;
    unsigned percent;
};

void battery_init(void);
/* Whether this board has a battery gauge (the phone; QEMU has none). */
bool battery_supported(void);
const struct battery_view *battery_view(void);
/* The screen going off and on: no polls while it is off. */
void battery_standby(bool active);
bool battery_pending(void);
void battery_service(void *);
#endif
