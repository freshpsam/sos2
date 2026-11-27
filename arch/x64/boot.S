/* x64 entry: QEMU's PVH loader starts the kernel at pvh_start in 32-bit
 * protected mode, paging off, interrupts masked, with EBX pointing to the
 * hvm_start_info (arch/x64/machine.c). It identity-maps the low 4 GiB
 * with 2 MiB pages, write-back below 3 GiB and uncached above it, where
 * the device MMIO lies, enters long mode and calls x86_boot (cpu.c) with the
 * start info. On a PC, SOS2's UEFI loader (boot/uefi/loader.c) enters at
 * uefi_start instead, in long mode on the firmware's identity map, which
 * the same tables replace, write-back throughout (the MTRRs decide).
 * arch/x64/mmu.c replaces them at vm_init.
 *
 * Secondary CPUs start in real mode at X86_TRAMPOLINE, where smp.c copies
 * x86_trampoline with the kernel's translation root in its last word: they
 * enter long mode on the kernel's own tables and find their record by APIC
 * ID, as arm64's boot.S finds it by MPIDR.
 * https://xenbits.xen.org/docs/unstable/misc/pvh.html */
#include "arch/smp.h"
#define X86_TRAMPOLINE 0x8000
#define EFER           0xc0000080
#define EFER_LME_NXE   0x900       /* long mode, no-execute */
#define CR0_PG_WP_PE   0x80010001
#define CR4_PAE        0x20

    .section .note.Xen, "a"
    .p2align 2
    .long 4, 4, 18                 /* namesz, descsz, XEN_ELFNOTE_PHYS32_ENTRY */
    .asciz "Xen"
    .long pvh_start

    .section .text.boot, "ax"
    .code32
    .global pvh_start
    .type pvh_start, @function
pvh_start:
    cli
    cld
    movl    %ebx, %esi              /* the start info */
    movl    $__stack_top, %esp
    /* PML4[0] -> PDPT; PDPT[0..3] -> four PDs of 512 2 MiB pages each:
     * present, writable, large; PCD|PWT (uncached) from 3 GiB. */
    movl    $boot_pdpt, %eax
    orl     $3, %eax
    movl    %eax, boot_pml4
    xorl    %ecx, %ecx
1:  movl    %ecx, %eax
    shll    $12, %eax
    addl    $boot_pd, %eax
    orl     $3, %eax
    movl    %eax, boot_pdpt(, %ecx, 8)
    incl    %ecx
    cmpl    $4, %ecx
    jne     1b
    xorl    %ecx, %ecx
2:  movl    %ecx, %eax
    shll    $21, %eax
    orl     $0x83, %eax
    cmpl    $1536, %ecx
    jb      3f
    orl     $0x18, %eax
3:  movl    %eax, boot_pd(, %ecx, 8)
    movl    $0, boot_pd + 4(, %ecx, 8)
    incl    %ecx
    cmpl    $2048, %ecx
    jne     2b
    movl    $boot_pml4, %eax
    movl    %eax, %cr3
    movl    %cr4, %eax
    orl     $CR4_PAE, %eax
    movl    %eax, %cr4
    movl    $EFER, %ecx
    rdmsr
    orl     $EFER_LME_NXE, %eax
    wrmsr
    movl    %cr0, %eax
    andl    $0x9fffffff, %eax       /* caches on: CD and NW clear */
    orl     $CR0_PG_WP_PE, %eax
    movl    %eax, %cr0
    lgdt    boot_gdt_pointer
    ljmp    $0x08, $long_entry

    .code64
long_entry:
    movl    $0x10, %eax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    xorl    %eax, %eax
    movw    %ax, %fs
    movw    %ax, %gs
    movl    %esi, %edi
    leaq    __stack_top(%rip), %rsp
    call    x86_boot
1:  hlt
    jmp     1b
    .size pvh_start, .-pvh_start

    /* A PC: SOS2's UEFI loader jumps here with interrupts masked, RDI
     * pointing to the boot information (include/boot/sos2_boot_info.h),
     * after leaving the firmware's boot services. The kernel's tables are
     * pvh_start's, built in long mode, but all write-back: a PC's RAM
     * reaches above 3 GiB, where the loader puts the resources, and the
     * firmware's MTRRs keep the MMIO below 4 GiB uncached. */
    .global uefi_start
    .type uefi_start, @function
uefi_start:
    cli
    cld
    movq    %rdi, %rbx
    leaq    __stack_top(%rip), %rsp
    leaq    boot_pml4(%rip), %rdi
    leaq    boot_pdpt(%rip), %rsi
    leaq    boot_pd(%rip), %rdx
    leaq    3(%rsi), %rax
    movq    %rax, (%rdi)
    xorl    %ecx, %ecx
1:  movq    %rcx, %rax
    shlq    $12, %rax
    leaq    3(%rdx, %rax), %rax
    movq    %rax, (%rsi, %rcx, 8)
    incl    %ecx
    cmpl    $4, %ecx
    jne     1b
    xorl    %ecx, %ecx
2:  movq    %rcx, %rax
    shlq    $21, %rax
    orq     $0x83, %rax
    movq    %rax, (%rdx, %rcx, 8)
    incl    %ecx
    cmpl    $2048, %ecx
    jne     2b
    movq    %rdi, %cr3
    lgdt    boot_gdt_pointer64(%rip)
    pushq   $0x08
    leaq    4f(%rip), %rax
    pushq   %rax
    lretq
4:  movl    $0x10, %eax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    xorl    %eax, %eax
    movw    %ax, %fs
    movw    %ax, %gs
    movq    %rbx, %rdi
    call    x86_boot
5:  hlt
    jmp     5b
    .size uefi_start, .-uefi_start

    /* A secondary CPU in long mode on the kernel's tables: its boot record
     * by initial APIC ID (smp.c's x86_cpu_boot), then x86_cpu_secondary on
     * its own stack. A CPU without a record halts. */
    .global x86_secondary_entry
    .type x86_secondary_entry, @function
x86_secondary_entry:
    movl    $0x10, %eax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movl    $1, %eax
    cpuid
    shrl    $24, %ebx
    leaq    x86_cpu_boot(%rip), %rax
    movl    $ARCH_CPU_MAX, %ecx
1:  cmpq    %rbx, (%rax)
    je      2f
    addq    $32, %rax
    decl    %ecx
    jnz     1b
3:  cli
    hlt
    jmp     3b
2:  movq    16(%rax), %rsp
    movq    8(%rax), %rdi
    testq   %rdi, %rdi
    jz      3b
    call    x86_cpu_secondary
    jmp     3b
    .size x86_secondary_entry, .-x86_secondary_entry

    /* The real-mode trampoline, which runs where smp.c copies it, at
     * X86_TRAMPOLINE: its addresses are X86_TRAMPOLINE plus their offset. */
#define T(label) (X86_TRAMPOLINE + (label - x86_trampoline))
    .section .rodata, "a"
    .p2align 4
    .global x86_trampoline, x86_trampoline_end
    .code16
x86_trampoline:
    cli
    cld
    movw    %cs, %ax
    movw    %ax, %ds
    lgdtl   (trampoline_gdt_pointer - x86_trampoline)
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0
    ljmpl   $0x08, $T(trampoline32)
    .code32
trampoline32:
    movl    $0x10, %eax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movl    %cr4, %eax
    orl     $CR4_PAE, %eax
    movl    %eax, %cr4
    movl    T(trampoline_root), %eax
    movl    %eax, %cr3
    movl    $EFER, %ecx
    rdmsr
    orl     $EFER_LME_NXE, %eax
    wrmsr
    movl    %cr0, %eax
    andl    $0x9fffffff, %eax
    orl     $CR0_PG_WP_PE, %eax
    movl    %eax, %cr0
    ljmpl   $0x18, $T(trampoline64)
    .code64
trampoline64:
    movabsq $x86_secondary_entry, %rax
    jmpq    *%rax
    /* Descriptors already marked accessed: the CPU would otherwise write
     * that bit into this page, which is read-only once paging is on. */
    .p2align 3
trampoline_gdt:
    .quad 0
    .quad 0x00cf9b000000ffff        /* 0x08: 32-bit code */
    .quad 0x00cf93000000ffff        /* 0x10: data */
    .quad 0x00209b0000000000        /* 0x18: 64-bit code */
trampoline_gdt_pointer:
    .word 4 * 8 - 1
    .long T(trampoline_gdt)
    .p2align 2
    /* The kernel's root (arch/x64/mmu.c), below 4 GiB; smp.c sets it. */
    .global x86_trampoline_root
x86_trampoline_root:
trampoline_root:
    .long 0
x86_trampoline_end:
#undef T

    .p2align 3
boot_gdt:
    .quad 0
    .quad 0x00209b0000000000        /* 0x08: 64-bit code, accessed */
    .quad 0x0000930000000000        /* 0x10: data, accessed */
boot_gdt_pointer:
    .word 3 * 8 - 1
    .long boot_gdt
    /* Long mode's form, for uefi_start. */
    .p2align 3
boot_gdt_pointer64:
    .word 3 * 8 - 1
    .quad boot_gdt

    .section .bss
    .p2align 12
    .global boot_pml4, boot_pdpt
boot_pml4: .skip 4096
boot_pdpt: .skip 4096
boot_pd:   .skip 4 * 4096
    .section .note.GNU-stack, "", @progbits
