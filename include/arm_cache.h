#ifndef SOS2_ARM_CACHE_H
#define SOS2_ARM_CACHE_H
#include "fdt.h"
#include "arch/cpu.h"

/* Whole-cache maintenance is restricted to boot, before releasing other CPUs. */
void arm_dcache_invalidate_all(void);
void arm_dcache_invalidate_local(void);
#ifndef __aarch64__
/* The Redmi 2's CPU power-down. */
void arm_dcache_clean_invalidate_local(void);
void arm_dcache_clean_invalidate_all(void);
#endif
void arm_cache_restore(uint32_t control);
uint32_t arm_cache_control(void);

/* Call only after validating the complete kernel, source, output and scanout
 * ranges. This temporary mapping ends before drivers or user processes start. */
bool arm_boot_cache_begin(const struct platform_devices *, uintptr_t dtb, uintptr_t source,
                          size_t source_size, uintptr_t output, size_t output_size);
void arm_boot_cache_end(void);
#ifdef SOS2_BOARD_NEXUS5
/* From the Nexus 5's entry (boot.S), after BSS is cleared: translation, the
 * instruction cache and branch prediction on behind tables of the kernel
 * alone (arch/arm/boot_cache.c); the data cache stays off. early: whether
 * those tables are in use, until vm_init installs the kernel's. */
void arm_early_mmu(void);
bool arm_boot_cache_early(void);
#else
static inline bool arm_boot_cache_early(void) { return false; }
#endif
#endif
