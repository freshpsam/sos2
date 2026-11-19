#ifndef SOS2_REDMI2_SMP_H
#define SOS2_REDMI2_SMP_H
#include <stdint.h>
#include <stdbool.h>
unsigned redmi2_smp_prepare(uint32_t boot_mpidr, uintptr_t entry, uintptr_t warm_entry);
bool redmi2_smp_release(unsigned cpu);
/* The CPUs whose warm-boot entry the firmware accepted. */
unsigned redmi2_smp_warm_mask(void);
#endif
