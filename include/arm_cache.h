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
#ifndef __aarch64__
/* Memory types under TEX remap (SCTLR.TRE), as Linux's ARMv7 setup: a
 * descriptor's TEX[0], C and B pick type n as they do without remap (0
 * Strongly-ordered, 1 Device, 2 write-through, 3 write-back, 4-6
 * non-cacheable, 7 write-back write-allocate), and shareable (S=1) Normal
 * memory is Inner Shareable. Without remap S=1 means Outer Shareable, which
 * Krait (the Nexus 5) keeps out of its L1 data cache. PRRR and NMRR are per
 * CPU; set them before translation. */
#define ARM_PRRR      0xff0aaaa4u
#define ARM_NMRR      0x40e040e0u
#define ARM_SCTLR_TRE (1u << 28)

static inline void arm_memory_types(void) {
    __asm__ volatile("mcr p15, 0, %0, c10, c2, 0\n mcr p15, 0, %1, c10, c2, 1\n isb"
                     :
                     : "r"(ARM_PRRR), "r"(ARM_NMRR)
                     : "memory");
}
#endif

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
