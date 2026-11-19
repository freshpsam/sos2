#include "arch/smp.h"
    .syntax unified
    .arm
    .section .text.boot, "ax", %progbits
    .global _start
    .type _start, %function
_start:
    b       boot_entry
    /* ARM zImage-shaped header, not a Linux decompressor. lk2nd loads this
     * raw image at 0x80008000 and calls it in ARM state with the DTB in r2.
     * Bytes 8..15 remain zero: lk2nd also examines the ARM64 text_offset there. */
    .space 0x24 - (. - _start), 0
    .word 0x016F2818
    .word 0
    .word __file_end - _start
    .space 0x40 - (. - _start), 0

boot_entry:
    mov     r9, #0
    mov     r8, r2
    b       boot_mode
    .global arm_secondary_entry
arm_secondary_entry:
    mov     r9, #1
boot_mode:
    mrs     r0, cpsr
    and     r0, r0, #0x1F
    cmp     r0, #0x13           @ Non-secure SVC is lk2nd's direct ARM32 path.
    beq     svc_entry
    cmp     r0, #0x1A           @ Also accept HYP for a virtualized loader.
    bne     unsupported_entry

    /* ARM boot protocol requires MMU and D-cache off. Do not discard dirty
     * cache lines by blindly clearing these bits in a nonconforming handoff. */
    mrc     p15, 4, r0, c1, c0, 0   @ HSCTLR: current HYP translation/cache state.
    tst     r0, #5
    bne     unsupported_entry
    ldr     r0, =hyp_vectors
    mcr     p15, 4, r0, c12, c0, 0  @ HVBAR, before changing HYP control registers.
    mov     r0, #0
    mcr     p15, 4, r0, c1, c1, 0   @ HCR: no stage 2, routing, or traps.
    mcr     p15, 4, r0, c1, c1, 2   @ HCPTR
    mcr     p15, 4, r0, c1, c1, 3   @ HSTR
    mcrr    p15, 4, r0, r0, c14     @ CNTVOFF: virtual and physical time agree.
    mrc     p15, 4, r0, c1, c1, 1   @ HDCR: retain only the implemented PMU count.
    and     r0, r0, #0x1F
    mcr     p15, 4, r0, c1, c1, 1
    mov     r0, #3
    mcr     p15, 4, r0, c14, c1, 0  @ CNTHCTL: permit PL1 physical timer accesses.
    mrc     p15, 0, r0, c0, c0, 0
    mcr     p15, 4, r0, c0, c0, 0   @ VPIDR: retain the real CPU identity.
    mrc     p15, 0, r0, c0, c0, 5
    mcr     p15, 4, r0, c0, c0, 5   @ VMPIDR
    ldr     r0, =0x1D3              @ ARM, little-endian SVC; A/I/F masked.
    msr     spsr_cxsf, r0
    adr     r0, svc_entry
    msr     ELR_hyp, r0
    isb
    eret

svc_entry:
    cpsid   aif
    mrc     p15, 0, r0, c1, c0, 0  @ SCTLR
    tst     r0, #5
    bne     unsupported_entry
    bic     r0, r0, #(1 << 13)     @ Use VBAR, not the legacy high-vector address.
    bic     r0, r0, #(1 << 25)     @ Little-endian exception data accesses.
    bic     r0, r0, #(1 << 30)     @ Exception handlers run in ARM state.
    /* Instruction caching works with translation disabled. Data caching waits
     * for audited RAM/MMIO mappings; never cache the inherited framebuffer. */
    mov     r1, #0
    dsb     sy
    mcr     p15, 0, r1, c7, c5, 0   @ ICIALLU
    mcr     p15, 0, r1, c7, c5, 6   @ BPIALL
    dsb     sy
    orr     r0, r0, #(1 << 12)
    mcr     p15, 0, r0, c1, c0, 0
    isb
    cmp     r9, #1
    beq     secondary_stacks
    cmp     r9, #2
    beq     cpu0_resume
    ldr     r0, =arm_cpus
    mcr     p15, 0, r0, c13, c0, 4  @ TPIDRPRW: privileged per-CPU continuation.
    cps     #0x12
    ldr     sp, =__irq_stack_top
    cps     #0x17
    ldr     sp, =__abort_stack_top
    cps     #0x1B
    ldr     sp, =__undef_stack_top
    cps     #0x13
    ldr     r0, =arm_process_vectors
    ldr     sp, =__stack_top
    mcr     p15, 0, r0, c12, c0, 0  @ VBAR
    isb
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r3, #0
    mov     r4, #0
    mov     r5, #0
    mov     r6, #0
    /* Both linker scripts align BSS boundaries to at least 16 bytes. */
1:  cmp     r0, r1
    bhs     2f
    stmia   r0!, {r3-r6}
    b       1b
2:  mov     r0, r8
    bl      kernel_main
    b       unsupported_entry

secondary_stacks:
    mrc     p15, 0, r0, c0, c0, 5
    bic     r0, r0, #0xFF000000
    ldr     r4, =arm_cpu_boot
    mov     r5, #ARCH_CPU_MAX
3:  ldr     r1, [r4]
    cmp     r0, r1
    beq     4f
    add     r4, r4, #32
    subs    r5, r5, #1
    bne     3b
    b       unsupported_entry
4:  cps     #0x12
    ldr     sp, [r4, #12]
    cps     #0x17
    ldr     sp, [r4, #16]
    cps     #0x1B
    ldr     sp, [r4, #20]
    cps     #0x13
    ldr     sp, [r4, #8]
    ldr     r0, [r4, #4]
    mcr     p15, 0, r0, c13, c0, 4
    ldr     r1, =arm_process_vectors
    mcr     p15, 0, r1, c12, c0, 0
    isb
    bl      arm_cpu_secondary

/* Warm entry after a power collapse, for every CPU (the firmware may keep one
 * warm-boot address): CPU 0, affinity 0 in MSM8916's single cluster, resumes
 * its saved context; the others restart as at cold boot. The firmware enters
 * it like a cold boot, with the MMU and caches off. */
    .global arm_warm_entry
    .type arm_warm_entry, %function
arm_warm_entry:
    mrc     p15, 0, r9, c0, c0, 5
    ands    r9, r9, #0xFF
    movne   r9, #1
    moveq   r9, #2
    b       boot_mode

    .equ CTX_CPSR, 0
    .equ CTX_SP, 4
    .equ CTX_SCTLR, 8
    .equ CTX_ACTLR, 12
    .equ CTX_CPACR, 16
    .equ CTX_TTBR0, 20
    .equ CTX_TTBCR, 24
    .equ CTX_DACR, 28
    .equ CTX_CONTEXTIDR, 32
    .equ CTX_CNTKCTL, 36
    .equ CTX_TPIDRURW, 40
    .equ CTX_TPIDRURO, 44

/* int arm_cpu0_suspend(void (*power_down)(void)): saves CPU 0's context and
 * calls power_down. Returns 1 if power_down returns (the CPU stayed on), or 0
 * once the CPU has powered down and resumed through arm_warm_entry, with its
 * callee-saved registers, stack, modes and translation as before. As Linux's
 * cpu_suspend: the context reaches memory before the power-down, and the
 * resume rebuilds the CPU's state from it. The stack frame itself is read back
 * only with caching on again, through the L2 that stays powered. Ten registers
 * keep the stack 8-byte aligned for the call into C (AAPCS). */
    .global arm_cpu0_suspend
    .type arm_cpu0_suspend, %function
arm_cpu0_suspend:
    push    {r4-r12, lr}
    ldr     r4, =arm_cpu0_context
    mrs     r1, cpsr
    str     r1, [r4, #CTX_CPSR]
    str     sp, [r4, #CTX_SP]
    mrc     p15, 0, r1, c1, c0, 0   @ SCTLR
    str     r1, [r4, #CTX_SCTLR]
    mrc     p15, 0, r1, c1, c0, 1   @ ACTLR
    str     r1, [r4, #CTX_ACTLR]
    mrc     p15, 0, r1, c1, c0, 2   @ CPACR
    str     r1, [r4, #CTX_CPACR]
    mrc     p15, 0, r1, c2, c0, 0   @ TTBR0
    str     r1, [r4, #CTX_TTBR0]
    mrc     p15, 0, r1, c2, c0, 2   @ TTBCR
    str     r1, [r4, #CTX_TTBCR]
    mrc     p15, 0, r1, c3, c0, 0   @ DACR
    str     r1, [r4, #CTX_DACR]
    mrc     p15, 0, r1, c13, c0, 1  @ CONTEXTIDR
    str     r1, [r4, #CTX_CONTEXTIDR]
    mrc     p15, 0, r1, c14, c1, 0  @ CNTKCTL
    str     r1, [r4, #CTX_CNTKCTL]
    mrc     p15, 0, r1, c13, c0, 2  @ TPIDRURW
    str     r1, [r4, #CTX_TPIDRURW]
    mrc     p15, 0, r1, c13, c0, 3  @ TPIDRURO
    str     r1, [r4, #CTX_TPIDRURO]
    /* The context is uncached memory; clean it to the point of coherency
     * anyway, so the resume never depends on that mapping. */
    dsb     sy
    mcr     p15, 0, r4, c7, c10, 1  @ DCCMVAC
    add     r1, r4, #32
    mcr     p15, 0, r1, c7, c10, 1
    dsb     sy
    blx     r0
    mov     r0, #1
    pop     {r4-r12, pc}

cpu0_resume:
    ldr     r0, =arm_cpus
    mcr     p15, 0, r0, c13, c0, 4       @ TPIDRPRW: CPU 0's record.
    cps     #0x12
    ldr     sp, =__irq_stack_top
    cps     #0x17
    ldr     sp, =__abort_stack_top
    cps     #0x1B
    ldr     sp, =__undef_stack_top
    cps     #0x13
    ldr     r0, =arm_process_vectors
    mcr     p15, 0, r0, c12, c0, 0       @ VBAR
    ldr     r4, =arm_cpu0_context
    ldr     r0, [r4, #CTX_ACTLR]
    mcr     p15, 0, r0, c1, c0, 1
    ldr     r0, [r4, #CTX_CPACR]
    mcr     p15, 0, r0, c1, c0, 2
    ldr     r0, [r4, #CTX_TTBCR]
    mcr     p15, 0, r0, c2, c0, 2
    ldr     r0, [r4, #CTX_DACR]
    mcr     p15, 0, r0, c3, c0, 0
    ldr     r0, [r4, #CTX_TTBR0]
    mcr     p15, 0, r0, c2, c0, 0
    ldr     r0, [r4, #CTX_CONTEXTIDR]
    mcr     p15, 0, r0, c13, c0, 1
    ldr     r0, [r4, #CTX_CNTKCTL]
    mcr     p15, 0, r0, c14, c1, 0
    ldr     r0, [r4, #CTX_TPIDRURW]
    mcr     p15, 0, r0, c13, c0, 2
    ldr     r0, [r4, #CTX_TPIDRURO]
    mcr     p15, 0, r0, c13, c0, 3
    mov     r0, #0
    mcr     p15, 0, r0, c8, c7, 0        @ TLBIALL
    dsb     sy
    isb
    ldr     sp, [r4, #CTX_SP]
    bl      arm_dcache_invalidate_local  @ The powered-up L1 holds nothing valid.
    ldr     r0, [r4, #CTX_SCTLR]
    dsb     sy
    mcr     p15, 0, r0, c1, c0, 0        @ MMU and caches as before.
    isb
    ldr     r0, [r4, #CTX_CPSR]
    msr     cpsr_cxsf, r0
    mov     r0, #0
    pop     {r4-r12, pc}

unsupported_entry:
    /* No verified display or console is available at this stage. */
    yield
    b       unsupported_entry

    .balign 32
hyp_vectors:
    /* Unexpected HYP exceptions cannot use the lower-mode exception ABI. */
    .rept 8
    b       unsupported_entry
    .endr
    .ltorg

    .section .bss
    .balign 64
    .global arm_cpu0_context
arm_cpu0_context:
    .space 64
    .section .note.GNU-stack, "", %progbits
