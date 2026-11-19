#ifndef SOS2_REDMI2_POWER_HW_H
#define SOS2_REDMI2_POWER_HW_H

#include <stdbool.h>
#include <stdint.h>

/* WT88047 only; caller must validate the board. Initialization reads the PON
 * key and verifies the inherited live PWM-to-MPP4 backlight route. It does not
 * program peripheral configuration. Reinitialization while blanked is refused. */
bool redmi2_power_hw_init(void);
/* Success and pressed state are separate; failure leaves *pressed unchanged. */
bool redmi2_power_hw_read_key(bool *pressed);
/* false gates the inherited PWM/MPP4 enable bits, then drives MPP4, the
 * backlight driver's enable, low (a digital output at a constant 0) instead
 * of leaving it undriven, as the vendor kernel leaves it. true puts MPP4's
 * PWM route back, restores the enable bits, forcibly rewrites the captured
 * duty after PWM enable, and triggers the PM8916 latch workaround.
 * Brightness, panel scanout, regulators and hardware long-press reset remain
 * unchanged. Failed off attempts restoration; true stays usable. */
bool redmi2_power_hw_set_backlight(bool enabled);
/* After init: the power key's PMIC interrupt on, both edges, through the SPMI
 * arbiter's shared line for this processor (GIC SPI 190); false if this
 * processor does not own it or a transfer fails. Rearm, before each wait for
 * it: the latched interrupt cleared, and any other PMIC interrupt of this
 * processor's that holds the line kept off it. */
bool redmi2_power_hw_key_irq(void);
bool redmi2_power_hw_key_rearm(void);
/* Whether the key latched an edge since the last rearm (in the arbiter or
 * the PMIC). */
bool redmi2_power_hw_key_latched(void);
/* After init: one register of any PMIC peripheral (ppid: its SID << 8 | its
 * address >> 8), read through the arbiter's observer channel as Linux reads
 * one (spmi-pmic-arb.c); false, quietly, if the arbiter has no channel for
 * it or the read fails. Nothing is written. */
bool redmi2_power_hw_read(unsigned ppid, uint8_t reg, uint8_t *value);
/* After init: one register of a PMIC peripheral written through this
 * processor's channel; false if the arbiter has no channel for it or denies
 * or fails the write. Nothing is read back. */
bool redmi2_power_hw_write(unsigned ppid, uint8_t reg, uint8_t value);

#endif
