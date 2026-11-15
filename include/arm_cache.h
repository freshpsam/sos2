#ifndef SOS2_ARM_CACHE_H
#define SOS2_ARM_CACHE_H
#include "fdt.h"

/* Whole-cache maintenance is restricted to boot, before releasing other CPUs. */
void arm_dcache_invalidate_all(void);
void arm_dcache_invalidate_local(void);
void arm_dcache_clean_invalidate_local(void);
void arm_dcache_clean_invalidate_all(void);
void arm_cache_restore(uint32_t control);
uint32_t arm_cache_control(void);

static inline void arm_instruction_sync(void) {
    uint32_t zero = 0;
    __asm__ volatile("dsb sy\n mcr p15, 0, %0, c7, c5, 0\n"
                     "mcr p15, 0, %0, c7, c5, 6\n dsb sy\n isb"
                     :
                     : "r"(zero)
                     : "memory");
}

/* Call only after validating the complete kernel, source, output and scanout
 * ranges. This temporary mapping ends before drivers or user processes start. */
bool arm_boot_cache_begin(const struct platform_devices *, uintptr_t dtb, uintptr_t source,
                          size_t source_size, uintptr_t output, size_t output_size);
void arm_boot_cache_end(void);
#endif
