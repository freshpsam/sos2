#ifndef SOS2_BATTERY_H
#define SOS2_BATTERY_H
#include "platform.h"

/* What the battery icon shows. charger: a charger is connected; charging: it
 * charges the battery; percent: the board's level, steadied (battery.c). */
struct battery_view {
    bool available, charger, charging;
    unsigned percent;
};

/* The level for a battery voltage in mV through a cell's open-circuit
 * voltages at each 5% of charge from 100% down: linear between them, 100
 * from the first up and 0 below the last. */
unsigned battery_ocv_percent(const uint16_t ocv[21], unsigned millivolts);

void battery_init(void);
const struct battery_view *battery_view(void);
/* The screen going off and on: no polls while it is off. */
void battery_standby(bool active);
bool battery_pending(void);
void battery_service(void *);
#endif
