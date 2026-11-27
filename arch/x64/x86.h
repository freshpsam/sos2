#ifndef SOS2_X86_H
#define SOS2_X86_H
/* What arch/x64's files share among themselves. */
#include "smp.h"

/* Where smp.c copies the secondary CPUs' real-mode trampoline (boot.S): a
 * page below 1 MiB, which machine.c withholds from the page pool. */
#define X86_TRAMPOLINE 0x8000

/* CPUID's answers that the kernel acts on (cpu.c). */
struct x86_features {
    bool x2apic, tsc_deadline, smep, smap, invariant_tsc;
};
extern struct x86_features x86_features;
/* SMAP is on: kernel entries from user mode clear EFLAGS.AC (entry.S). */
extern unsigned char x86_smap;

/* Each CPU's exception stack (arch/smp.h) and the stack of the faults that
 * stop it at once (NMI, double fault, machine check): both above an unmapped
 * guard page. */
#define X86_FATAL_STACK_BYTES (2 * 4096)
extern unsigned char x86_exception_stacks[CPU_MAX][ARCH_EXCEPTION_STACK_BYTES];
extern unsigned char x86_fatal_stacks[CPU_MAX][X86_FATAL_STACK_BYTES];
extern struct cpu_boot_record x86_cpu_boot[CPU_MAX];
extern const unsigned char x86_trampoline[], x86_trampoline_end[], x86_trampoline_root[];
extern const unsigned char x86_trap_stubs[];

/* machine.c: the IOAPICs ACPI lists, whose inputs apic.c masks. */
#define X86_IOAPICS 4
extern uintptr_t x86_ioapics[X86_IOAPICS];
extern unsigned x86_ioapic_count;

/* cpu.c: this CPU's descriptor tables, system registers and MSRs. */
void x86_cpu_setup(struct cpu_record *cpu);
/* apic.c: the TSC's frequency, then the local APIC. */
extern uint64_t x86_tsc_frequency;
bool x86_tsc_calibrate(void);
void x86_interrupt_eoi(void);
uint32_t x86_apic_id(void);
/* An IPI to apic_id: X86_IPI_INIT, X86_IPI_STARTUP(page) or a fixed vector. */
#define X86_IPI_INIT          0x4500u
#define X86_IPI_STARTUP(page) (0x4600u | (page))
void x86_send_ipi(uint32_t apic_id, uint32_t command);
/* smp.c: a secondary CPU's kernel fault, recorded for CPU 0, which panics. */
_Noreturn void x86_cpu_failed(struct cpu_record *cpu);
/* mmu.c: the kernel's translation root, for the trampoline; and the
 * trampoline's page, writable while smp.c copies it, then executable. */
uintptr_t x86_kernel_root(void);
void x86_trampoline_page(bool writable);
#endif
