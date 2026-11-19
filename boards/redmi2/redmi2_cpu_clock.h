#ifndef SOS2_REDMI2_CPU_CLOCK_H
#define SOS2_REDMI2_CPU_CLOCK_H
#include <stdbool.h>
#include <stdint.h>

/* Standby runs the CPU cluster at 200 MHz (GPLL0 / 4, the lowest rate in
 * Linux's msm8916 CPU table); waking restores the divider lk2nd chose
 * (GPLL0 / 1, 800 MHz). The voltage is left alone: a lower clock at the same
 * voltage stays within the part's limits. A pending change or a source other
 * than GPLL0 keeps its rate, and waking keeps 200 MHz if the mux does not
 * confirm the restore. */
void redmi2_cpu_clock_standby(bool slow);
/* Across a whole-chip sleep, as the vendor's ramp_up_first_cpu rewrites the
 * mux: its configuration before, and after, a changed one written back. */
uint32_t redmi2_cpu_clock_config(void);
void redmi2_cpu_clock_reapply(uint32_t cfg);

#endif
