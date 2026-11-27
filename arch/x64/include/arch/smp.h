#ifndef SOS2_ARCH_SMP_H
#define SOS2_ARCH_SMP_H

/* CPUs the kernel runs. */
#define ARCH_CPU_MAX 8

/* Each CPU's exception stack, whose lowest page is an unmapped guard: the
 * IST stack of every exception, and the stack interrupts from user mode
 * arrive on (TSS RSP0), as arm64's exception stacks. ARCH_CPU_* are offsets in
 * struct cpu_record (include/smp.h), which the assembly reads through GS:
 * the stack's top, the record itself and its index. */
#define ARCH_EXCEPTION_STACK_BYTES (4 * 4096)
#define ARCH_CPU_EXCEPTION_SP      8
#define ARCH_CPU_SELF              16
#define ARCH_CPU_INDEX             24
/* struct cpu_record's trap, which entry.S fills (struct cpu_trap). */
#define ARCH_CPU_TRAP 720

/* Interrupt vectors: the scheduler's timer, the wake an idle CPU halts for
 * (smp.c) and the local APIC's spurious one. The kernel takes no other. */
#define X86_TIMER_VECTOR    0x20
#define X86_WAKE_VECTOR     0xf0
#define X86_SPURIOUS_VECTOR 0xff
/* GDT selectors (cpu.c): the kernel's code and data, then the user's data
 * and code at RPL 3, as SYSCALL and SYSRET's STAR layout wants them. */
#define X86_KERNEL_CS 0x08
#define X86_KERNEL_DS 0x10
#define X86_USER_DS   0x1b
#define X86_USER_CS   0x23
#define X86_TSS       0x28

#ifndef __ASSEMBLER__
#include <stdint.h>

struct cpu_record;

/* Fixed 32-byte records arch/x64/boot.S reads on a secondary CPU's first
 * entry: its APIC ID, its record and the top of its stack. */
struct cpu_boot_record {
    uint64_t mpidr;
    struct cpu_record *cpu;
    uintptr_t sp;
    uint64_t reserved;
};

/* stack: the CPU's CPU_STACK_BYTES, whose lowest page is an unmapped guard
 * (arch/x64/mmu.c). */
#define CPU_BOOT_RECORD(id, cpu, stack) \
    ((struct cpu_boot_record){(id), (cpu), (stack) + 8 * 4096, 0})
#endif
#endif
