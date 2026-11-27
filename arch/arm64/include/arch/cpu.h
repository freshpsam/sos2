#ifndef SOS2_ARCH_CPU_H
#define SOS2_ARCH_CPU_H
#include <stdbool.h>
#include <stdint.h>

/* AArch64 primitives for the shared kernel code; arch/arm has the AArch32
 * versions of the same functions. */

static inline void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }

/* Orders this CPU's memory accesses for the other CPUs (inner shareable). */
static inline void arch_memory_barrier(void) { __asm__ volatile("dmb ish" ::: "memory"); }

static inline void arch_wait_event(void) { __asm__ volatile("wfe"); }

static inline void arch_yield(void) { __asm__ volatile("yield"); }

/* MPIDR_EL1's affinity fields 0-2: zero on the boot CPU. */
static inline uint32_t arch_cpu_affinity(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xffffff);
}

/* The generic timer's virtual count, not read ahead of earlier instructions,
 * and its frequency. */
static inline uint64_t arch_counter(void) {
    uint64_t count;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(count)::"memory");
    return count;
}

static inline uint32_t arch_counter_frequency(void) {
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    return (uint32_t)frequency;
}

/* The generic timer's virtual count, read after all earlier instructions. */
static inline uint64_t arch_counter_synchronized(void) { return arch_counter(); }

/* The virtual timer's compare value and control (CNTV_CVAL_EL0,
 * CNTV_CTL_EL0), and the kernel's timer control (CNTKCTL_EL1). */
static inline uint64_t arch_timer_compare(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, cntv_cval_el0" : "=r"(value));
    return value;
}

static inline void arch_timer_set_compare(uint64_t value) {
    __asm__ volatile("msr cntv_cval_el0, %0\n isb" : : "r"(value) : "memory");
}

static inline uint32_t arch_timer_control(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, cntv_ctl_el0" : "=r"(value));
    return (uint32_t)value;
}

static inline void arch_timer_set_control(uint32_t value) {
    __asm__ volatile("msr cntv_ctl_el0, %0\n isb" : : "r"((uint64_t)value) : "memory");
}

static inline uint32_t arch_timer_kernel_control(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, cntkctl_el1" : "=r"(value));
    return (uint32_t)value;
}

static inline void arch_timer_set_kernel_control(uint32_t value) {
    __asm__ volatile("msr cntkctl_el1, %0\n isb" : : "r"((uint64_t)value) : "memory");
}

/* Whether this CPU runs the kernel (EL1) with IRQs and FIQs masked. */
static inline bool arch_kernel_masked(void) {
    uint64_t daif, level;
    __asm__ volatile("mrs %0, daif\n mrs %1, currentel" : "=r"(daif), "=r"(level));
    return level == (1u << 2) && (daif & 0xc0) == 0xc0;
}

/* Invalidates this CPU's instruction cache after code changes. */
static inline void arm_instruction_sync(void) {
    __asm__ volatile("dsb sy\n ic iallu\n dsb sy\n isb" ::: "memory");
}

/* A translation table entry (arch/arm64/mmu.c). */
typedef uint64_t pte_t;

struct cpu_record;

/* This CPU's record (TPIDR_EL1). */
static inline struct cpu_record *arch_cpu_local(void) {
    struct cpu_record *cpu;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(cpu));
    return cpu;
}
#endif
