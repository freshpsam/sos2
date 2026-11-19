#ifndef SOS2_REDMI2_VIBRATOR_H
#define SOS2_REDMI2_VIBRATOR_H
#include <stdbool.h>

/* The drive's range, in 100 mV steps. */
#define REDMI2_VIBRATOR_MIN_MV 1200
#define REDMI2_VIBRATOR_MAX_MV 3100

/* The PM8916's vibrator driver, in manual mode. After redmi2_power_init: init
 * stops the motor, then sets its drive to millivolts. False (a value off the
 * steps, no channel, a write that does not read back, or a PWM line driving
 * the motor) leaves it unused for the boot: run must not be called then. run
 * switches the motor on or off; a stop is true only once the register reads
 * back as written. */
bool redmi2_vibrator_init(unsigned millivolts);
bool redmi2_vibrator_run(bool on);
#endif
