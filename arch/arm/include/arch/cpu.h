#ifndef SOS2_ARCH_CPU_H
#define SOS2_ARCH_CPU_H
#include <stdbool.h>
#include <stdint.h>

/* AArch32 (ARMv7-A) primitives for the shared kernel code; arch/arm64 has the
 * AArch64 versions of the same functions. */

static inline void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* Orders this CPU's memory accesses for the other CPUs (inner shareable). */
static inline void arch_memory_barrier(void) { __asm__ volatile("dmb ish" ::: "memory"); }

static inline void arch_wait_event(void) { __asm__ volatile("wfe"); }

static inline void arch_yield(void) { __asm__ volatile("yield"); }

/* MPIDR's affinity fields: zero on the boot CPU. */
static inline uint32_t arch_cpu_affinity(void) {
    uint32_t mpidr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return mpidr & 0xffffff;
}

/* The generic timer's virtual count, read in one atomic access, and its
 * frequency. */
static inline uint64_t arch_counter(void) {
    uint32_t low, high;
    __asm__ volatile("mrrc p15, 1, %0, %1, c14" : "=r"(low), "=r"(high));
    return ((uint64_t)high << 32) | low;
}

static inline uint32_t arch_counter_frequency(void) {
    uint32_t frequency;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(frequency));
    return frequency;
}

/* The generic timer's virtual count, read after all earlier instructions. */
static inline uint64_t arch_counter_synchronized(void) {
    uint32_t low, high;
    __asm__ volatile("isb\n mrrc p15, 1, %0, %1, c14" : "=r"(low), "=r"(high));
    return ((uint64_t)high << 32) | low;
}

/* The virtual timer's compare value and control (CNTV_CVAL, CNTV_CTL), and the
 * kernel's timer control (CNTKCTL). */
static inline uint64_t arch_timer_compare(void) {
    uint32_t low, high;
    __asm__ volatile("mrrc p15, 3, %0, %1, c14" : "=r"(low), "=r"(high));
    return ((uint64_t)high << 32) | low;
}

static inline void arch_timer_set_compare(uint64_t value) {
    __asm__ volatile("mcrr p15, 3, %0, %1, c14\n isb"
                     :
                     : "r"((uint32_t)value), "r"((uint32_t)(value >> 32))
                     : "memory");
}

static inline uint32_t arch_timer_control(void) {
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c14, c3, 1" : "=r"(value));
    return value;
}

static inline void arch_timer_set_control(uint32_t value) {
    __asm__ volatile("mcr p15, 0, %0, c14, c3, 1\n isb" : : "r"(value) : "memory");
}

static inline uint32_t arch_timer_kernel_control(void) {
    uint32_t value;
    __asm__ volatile("mrc p15, 0, %0, c14, c1, 0" : "=r"(value));
    return value;
}

static inline void arch_timer_set_kernel_control(uint32_t value) {
    __asm__ volatile("mcr p15, 0, %0, c14, c1, 0\n isb" : : "r"(value) : "memory");
}

/* Whether this CPU runs the kernel (SVC mode) with IRQs and FIQs masked. */
static inline bool arch_kernel_masked(void) {
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    return (cpsr & 0xdf) == 0xd3;
}

/* Invalidates this CPU's instruction cache and branch predictor after code
 * changes. */
static inline void arm_instruction_sync(void) {
    uint32_t zero = 0;
    __asm__ volatile("dsb sy\n mcr p15, 0, %0, c7, c5, 0\n"
                     "mcr p15, 0, %0, c7, c5, 6\n dsb sy\n isb"
                     :
                     : "r"(zero)
                     : "memory");
}

/* A translation table entry (arch/arm/mmu.c). */
typedef uint32_t pte_t;

struct arm_cpu;

/* This CPU's record (TPIDRPRW). */
static inline struct arm_cpu *arch_cpu_local(void) {
    struct arm_cpu *cpu;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 4" : "=r"(cpu));
    return cpu;
}
#endif
