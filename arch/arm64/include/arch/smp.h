#ifndef SOS2_ARCH_SMP_H
#define SOS2_ARCH_SMP_H

/* CPUs the kernel runs; arch/arm64/boot.S searches this many boot records. */
#define ARCH_CPU_MAX 8

/* Each CPU's exception stack, whose lowest page is an unmapped guard.
 * Kernel exceptions are fatal and run on it (arch/arm64/entry.S), out of
 * reach of an overflow of the stack that faulted. ARCH_CPU_EXCEPTION_SP is
 * the offset of its top in struct arm_cpu (include/arm_smp.h). */
#define ARCH_EXCEPTION_STACK_BYTES (4 * 4096)
#define ARCH_CPU_EXCEPTION_SP      8

#ifndef __ASSEMBLER__
#include <stdint.h>

struct arm_cpu;

/* Fixed 32-byte records arch/arm64/boot.S reads with translation off on a
 * secondary CPU's first entry: its MPIDR affinity, its record and the top of
 * its stack. */
struct cpu_boot_record {
    uint64_t mpidr;
    struct arm_cpu *cpu;
    uintptr_t sp;
    uint64_t reserved;
};

/* stack: the CPU's ARM_CPU_STACK_BYTES, whose lowest page is an unmapped
 * guard (arch/arm64/mmu.c). */
#define CPU_BOOT_RECORD(id, cpu, stack) \
    ((struct cpu_boot_record){(id), (cpu), (stack) + 8 * 4096, 0})
#endif
#endif
