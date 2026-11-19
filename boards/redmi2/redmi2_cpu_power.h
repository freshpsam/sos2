#ifndef SOS2_REDMI2_CPU_POWER_H
#define SOS2_REDMI2_CPU_POWER_H
#include <stdbool.h>
#include <stdint.h>

/* Powering idle CPUs down: standalone power collapse, as the vendor kernel
 * hotplugs or idles a core and mainline Linux idles one on 32-bit MSM8916. A
 * powered-down secondary keeps no state; an interrupt (SGI 0) powers it up into
 * the warm-boot entry, which restarts it like a cold secondary. CPU 0 keeps
 * its context and resumes where it powered down. */

/* Programs the power controllers of the CPUs in cpu_mask; returns the CPUs
 * ready to power down. */
unsigned redmi2_cpu_power_init(unsigned cpu_mask);
/* On an idle secondary CPU, with its timer stopped: power it down. Returns
 * only if the firmware kept it on, e.g. for a pending interrupt. */
void redmi2_cpu_power_off(unsigned cpu);
/* On a CPU back from a power-down, first thing (also with the MMU off):
 * standby as its power mode again, and no further wake-up SGIs reach it. */
void redmi2_cpu_power_resumed(unsigned cpu);
/* From the boot CPU: whether a secondary has collapsed, and waking them. */
bool redmi2_cpu_power_collapsed(unsigned cpu);
void redmi2_cpu_power_wake(unsigned cpu_mask);

/* CPU 0: after redmi2_cpu_power_init readied it, sets up its wake-up timer.
 * redmi2_cpu0_sleep powers it down until about `microseconds` have passed and
 * returns true once it resumes; false means it stayed on (not ready, or the
 * firmware kept it on) and the caller waits another way. */
bool redmi2_cpu0_power_init(void);
bool redmi2_cpu0_sleep(uint32_t microseconds);
/* The wake-up timer's interrupt ID while CPU 0 can power down, else 0. */
unsigned redmi2_cpu0_wake_irq(void);
/* One more level-triggered SPI that wakes CPU 0 from a power-down (0: none),
 * enabled only while it is down and only if its line is low then. Whether
 * its line is up now, and whether it was up on the way into or out of the
 * last power-down. */
void redmi2_cpu0_wake_source(unsigned irq);
bool redmi2_cpu0_source_raised(void);
bool redmi2_cpu0_source_seen(void);

/* The L2 cache off with CPU 0 (the vendor's l2-pc level): after
 * redmi2_cpu0_power_init, programs the L2's power controller and routes the
 * RPM's wake interrupt. redmi2_cpu0_deep_sleep then cleans every cache level
 * and powers CPU 0 down with the L2 off, handing the chip to the RPM: 1 back
 * from the power-down, 0 the firmware kept it on, -1 not ready. The caller
 * has powered the secondaries down and written the MPM's wake-up. The
 * physical count of frame 0 (CNTPCT), for the MPM's timer; 0 if it cannot
 * be read. */
bool redmi2_l2_power_init(void);
int redmi2_cpu0_deep_sleep(uint32_t microseconds);
uint64_t redmi2_cpu0_count(void);

#endif
