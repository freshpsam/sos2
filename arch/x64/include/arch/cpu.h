#ifndef SOS2_ARCH_CPU_H
#define SOS2_ARCH_CPU_H
#include <stdbool.h>
#include <stdint.h>
#include "arch/smp.h"

/* x64 primitives for the shared kernel code; arch/arm and arch/arm64 have
 * the ARM versions of the same functions. x86 orders ordinary memory
 * accesses itself (TSO) and its DMA is cache-coherent, so most of what ARM
 * fences only needs the compiler held back. */

static inline void barrier(void) { __asm__ volatile("mfence" ::: "memory"); }

/* Orders this CPU's memory accesses for the other CPUs. */
static inline void arch_memory_barrier(void) { __asm__ volatile("mfence" ::: "memory"); }

/* A spin loop's hint; nothing on x86 waits for another CPU's store. */
static inline void arch_wait_event(void) { __asm__ volatile("pause" ::: "memory"); }

static inline void arch_yield(void) { __asm__ volatile("pause" ::: "memory"); }

/* The counter: the TSC shifted right by 4, so that its frequency fits the
 * shared 32-bit type for any TSC (AMD's run at 4.5 GHz and more). 16 TSC
 * ticks are about 5 ns. LFENCE keeps the read after earlier instructions. */
#define ARCH_COUNTER_SHIFT 4
extern uint32_t x86_counter_frequency;

static inline uint64_t x86_rdtsc(void) {
    uint32_t low, high;
    __asm__ volatile("lfence\n rdtsc" : "=a"(low), "=d"(high)::"memory");
    return ((uint64_t)high << 32) | low;
}

static inline uint64_t arch_counter(void) { return x86_rdtsc() >> ARCH_COUNTER_SHIFT; }

static inline uint32_t arch_counter_frequency(void) { return x86_counter_frequency; }

static inline uint64_t arch_counter_synchronized(void) { return arch_counter(); }

/* This CPU's index in cpu_records (include/smp.h), from its record: zero on
 * the boot CPU, whatever its APIC ID. */
static inline uint32_t arch_cpu_affinity(void) {
    uint32_t index;
    __asm__ volatile("movl %%gs:%c1, %0" : "=r"(index) : "i"(ARCH_CPU_INDEX));
    return index;
}

/* Whether this CPU runs the kernel (CPL 0) with interrupts masked. */
static inline bool arch_kernel_masked(void) {
    uint64_t flags;
    uint16_t cs;
    __asm__ volatile("pushfq\n popq %0\n movw %%cs, %1" : "=r"(flags), "=r"(cs));
    return !(cs & 3) && !(flags & 0x200);
}

/* A translation table entry (arch/x64/mmu.c). */
typedef uint64_t pte_t;

struct cpu_record;

/* This CPU's record: GS's base while the kernel runs. */
static inline struct cpu_record *arch_cpu_local(void) {
    struct cpu_record *cpu;
    __asm__ volatile("movq %%gs:%c1, %0" : "=r"(cpu) : "i"(ARCH_CPU_SELF));
    return cpu;
}

/* Model-specific registers, CPUID and I/O ports for arch/x64. */
static inline uint64_t x86_rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void x86_wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                     : "memory");
}

static inline void x86_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]) {
    __asm__ volatile("cpuid"
                     : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3])
                     : "a"(leaf), "c"(subleaf));
}

static inline void x86_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t x86_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline void x86_outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint16_t x86_inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}
#endif
