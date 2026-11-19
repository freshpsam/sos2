#include "arch/smp.h"
/* SCTLR_EL1's RES1 bits in ARMv8.0, with the MMU, caches and alignment checks
 * off; arch/arm64/mmu.c turns translation and data caching on. */
#define SCTLR_EL1_RES1 0x30d00800

    .section .text.boot, "ax", %progbits
    .global _start
    .type _start, %function
_start:
    /* Linux arm64 Image header: QEMU and Android bootloaders load the kernel
     * at a 2 MiB aligned RAM base plus text_offset and enter it with the DTB
     * in x0. */
    b       boot_entry
    .long   0
    .quad   0x80000             /* text_offset */
    .quad   __image_size        /* image_size, BSS and stacks included */
    .quad   2                   /* flags: little-endian, 4 KiB pages, near the RAM base */
    .quad   0, 0, 0
    .ascii  "ARM\x64"
    .long   0

boot_entry:
    mov     x19, x0
    mov     x20, #0
    b       boot_mode
    .global arm_secondary_entry
    .type arm_secondary_entry, %function
arm_secondary_entry:
    mov     x20, #1
boot_mode:
    msr     daifset, #0xf
    mrs     x0, currentel
    lsr     x0, x0, #2
    cmp     x0, #1
    b.eq    el1_entry
    cmp     x0, #2
    b.ne    unsupported_entry

    /* The boot protocol requires the MMU and data cache off. Do not discard
     * dirty lines by blindly clearing these bits in a nonconforming handoff. */
    mrs     x0, sctlr_el2
    mov     x1, #5              /* M and C */
    tst     x0, x1
    b.ne    unsupported_entry
    adr     x0, el2_vectors
    msr     vbar_el2, x0
    mov     x0, #(1 << 31)      /* HCR_EL2.RW: EL1 runs AArch64; no stage 2, routing or traps. */
    msr     hcr_el2, x0
    mov     x0, #3              /* CNTHCTL_EL2: EL1 may use the physical counter and timer. */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr    /* Virtual and physical time agree. */
    mov     x0, #0x33ff         /* CPTR_EL2: RES1 bits only, so no FP/SIMD traps. */
    msr     cptr_el2, x0
    msr     hstr_el2, xzr
    mrs     x0, id_aa64pfr0_el1 /* GICv3 system registers: let EL1 use its CPU interface. */
    ubfx    x0, x0, #24, #4
    cbz     x0, 1f
    mrs     x0, icc_sre_el2
    orr     x0, x0, #0xf        /* SRE, DFB, DIB and Enable */
    msr     icc_sre_el2, x0
    isb
    msr     ich_hcr_el2, xzr    /* No virtual CPU interface. */
1:
    mrs     x0, midr_el1        /* Retain the real CPU identity. */
    msr     vpidr_el2, x0
    mrs     x0, mpidr_el1
    msr     vmpidr_el2, x0
    ldr     x0, =SCTLR_EL1_RES1
    msr     sctlr_el1, x0
    mov     x0, #0x3c5          /* EL1h with D, A, I and F masked. */
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    isb
    eret

el1_entry:
    msr     spsel, #1
    mrs     x0, sctlr_el1
    mov     x1, #5
    tst     x0, x1
    b.ne    unsupported_entry
    /* Instruction caching works with translation off. Data caching waits for
     * audited RAM/MMIO mappings; never cache the inherited framebuffer. */
    ldr     x1, =(SCTLR_EL1_RES1 | (1 << 12))
    dsb     sy
    ic      iallu
    dsb     sy
    msr     sctlr_el1, x1
    isb
    cbnz    x20, secondary_stacks
    ldr     x0, =arm_cpus
    msr     tpidr_el1, x0       /* This CPU's record. */
    ldr     x0, =arm_process_vectors
    msr     vbar_el1, x0
    ldr     x0, =__stack_top
    mov     sp, x0
    isb
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    /* The linker script aligns BSS boundaries to 16 bytes. */
1:  cmp     x0, x1
    b.hs    2f
    stp     xzr, xzr, [x0], #16
    b       1b
2:  mov     x0, x19
    bl      kernel_main
    b       unsupported_entry

    /* Find this CPU's record (struct cpu_boot_record, arch/arm64/include/arch/smp.h). */
secondary_stacks:
    mrs     x0, mpidr_el1
    and     x0, x0, #0xffffff
    ldr     x4, =arm_cpu_boot
    mov     x5, #ARCH_CPU_MAX
3:  ldr     x1, [x4]
    cmp     x0, x1
    b.eq    4f
    add     x4, x4, #32
    subs    x5, x5, #1
    b.ne    3b
    b       unsupported_entry
4:  ldr     x1, [x4, #16]
    mov     sp, x1
    ldr     x0, [x4, #8]
    msr     tpidr_el1, x0
    ldr     x1, =arm_process_vectors
    msr     vbar_el1, x1
    isb
    bl      arm_cpu_secondary
    b       unsupported_entry

unsupported_entry:
    /* No verified display or console is available at this stage. */
    wfe
    b       unsupported_entry

    /* Unexpected EL2 exceptions cannot use the EL1 exception ABI. */
    .balign 2048
el2_vectors:
    .rept 16
    b       unsupported_entry
    .balign 128
    .endr
    .ltorg
    .section .note.GNU-stack, "", %progbits
