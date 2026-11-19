#ifndef SOS2_ARCH_SMP_H
#define SOS2_ARCH_SMP_H

/* CPUs the kernel runs; arch/arm/boot.S searches this many boot records. */
#define ARCH_CPU_MAX 4

#ifndef __ASSEMBLER__
#include <stdint.h>

struct arm_cpu;

/* Fixed 32-byte records arch/arm/boot.S reads with translation off on a
 * secondary CPU's first entry: its MPIDR affinity, its record, and the tops of
 * its SVC, IRQ, abort and undefined-mode stacks. */
struct cpu_boot_record {
    uint32_t mpidr;
    struct arm_cpu *cpu;
    uintptr_t svc, irq, abt, und;
    uint32_t reserved[2];
};

/* stack: the CPU's ARM_CPU_STACK_BYTES; each mode has two pages, the lower one
 * an unmapped guard (arch/arm/mmu.c). */
#define CPU_BOOT_RECORD(id, cpu, stack) \
    ((struct cpu_boot_record){(id), \
                              (cpu), \
                              (stack) + 2 * 4096, \
                              (stack) + 4 * 4096, \
                              (stack) + 6 * 4096, \
                              (stack) + 8 * 4096, \
                              {0, 0}})
#endif
#endif
