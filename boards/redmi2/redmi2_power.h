#ifndef SOS2_REDMI2_POWER_H
#define SOS2_REDMI2_POWER_H
#include <stdbool.h>

enum redmi2_power_change {
    REDMI2_POWER_UNCHANGED,
    REDMI2_POWER_SCREEN_OFF,
    REDMI2_POWER_SCREEN_ON,
    REDMI2_POWER_ERROR,
};

/* Call after WT88047 handoff validation. A boot-held key must be released;
 * subsequent debounced presses toggle once, without changing PMIC reset policy.
 * Repeated init preserves an active policy; a pending restore must finish first. */
bool redmi2_power_init(void);
enum redmi2_power_change redmi2_power_poll(void);
/* Software activity state. On a hardware error, fail awake and try to restore
 * the inherited backlight. */
bool redmi2_power_screen_on(void);
/* The power key's interrupt (the SPMI arbiter's line, GIC SPI 190) once init
 * armed it, else 0. Quiet, before a long standby sleep: no press or release
 * is being debounced, the interrupt is rearmed and the key still reads as
 * last sampled, so the next press or release raises that line. */
unsigned redmi2_power_key_irq(void);
bool redmi2_power_quiet(void);
/* Whether the key latched an edge since the interrupt was last rearmed; and,
 * once such an edge never raised the interrupt's line, no longer relying on
 * the interrupt: standby polls the key every 20 ms. */
bool redmi2_power_key_latched(void);
void redmi2_power_key_missed(void);
/* Restart the phone with a PMIC hard reset, as lk2nd reboots: the boot chain
 * starts again and lk2nd boots its boot partition. Returns false, with the
 * phone still running, if the PMIC refused a write or no reset came. */
bool redmi2_power_restart(void);
#endif
