#ifndef SOS2_REDMI2_CHIP_SLEEP_H
#define SOS2_REDMI2_CHIP_SLEEP_H
#include <stdbool.h>
#include <stdint.h>

/* Whole-chip sleep in standby: the L2 cache goes off with CPU 0 and the RPM
 * takes the chip, turning the crystal off and lowering the core rails, and
 * wakes it through the MPM on its timer or the power key. Init after CPU 0's
 * power-down and the power key's interrupt are ready. Standby entry requests
 * the sleep set. redmi2_chip_sleep sleeps once that way, for about
 * `microseconds`: true once it slept, or if the power key's line came up as
 * it went down (the caller polls the key at once); false means it did not,
 * and the caller sleeps as usual. */
bool redmi2_chip_sleep_init(void);
void redmi2_chip_sleep_standby(bool on);
bool redmi2_chip_sleep(uint32_t microseconds);

#endif
