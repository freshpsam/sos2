/* AArch64 exceptions, user slices and kernel context switches.
 *
 * Kernel services are serialized on the boot CPU. A user slice is a bounded
 * call, arm_user_run, which saves the kernel's callee-saved registers on
 * SP_EL1 and enters EL0; every exception from EL0 then builds a struct
 * cpu_trap (arch/arm64/include/arch/context.h) below them on the same stack,
 * calls arm_trap_dispatch and returns from arm_user_run. The kernel runs with
 * D, A, I and F masked, so an exception taken at EL1 is a fault, which
 * arm_trap_dispatch reports, on the CPU's exception stack.
 */
#include "arch/smp.h"
    .arch armv8-a+fp+simd
    .text

    .macro save_frame
    sub     sp, sp, #304
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x0, sp_el0
    stp     x30, x0, [sp, #240]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x0, x1, [sp, #256]
    mrs     x1, esr_el1
    mrs     x2, far_el1
    stp     x1, x2, [sp, #280]
    .endm

    /* x0: arch/arm's vector code: 1 undefined or other, 2 SVC, 3 instruction
     * abort, 4 data abort, 5 SError, 6 IRQ, 7 FIQ. */
    .macro trap name, code
\name:
    save_frame
    mov     x0, #\code
    b       dispatch
    .endm

    .balign 2048
    .global arm_process_vectors
arm_process_vectors:
    /* Current EL with SP_EL0, then with SP_ELx: kernel faults. */
    .rept 8
    b       kernel_exception
    .balign 128
    .endr
    /* Lower EL in AArch64: user exceptions. */
    b       user_sync
    .balign 128
    b       user_irq
    .balign 128
    b       user_fiq
    .balign 128
    b       user_serror
    .balign 128
    /* Lower EL in AArch32: never entered. */
    .rept 4
    b       kernel_exception
    .balign 128
    .endr

user_sync:
    save_frame
    lsr     x3, x1, #26         /* ESR_EL1.EC */
    mov     x0, #1
    mov     x4, #2
    cmp     x3, #0x15           /* SVC from AArch64 */
    csel    x0, x4, x0, eq
    mov     x4, #3
    cmp     x3, #0x20           /* Instruction abort from EL0 */
    csel    x0, x4, x0, eq
    mov     x4, #4
    cmp     x3, #0x24           /* Data abort from EL0 */
    csel    x0, x4, x0, eq
    b       dispatch
    trap    user_irq, 6
    trap    user_fiq, 7
    trap    user_serror, 5

    /* A kernel exception never returns, and the stack that faulted may have
     * overflowed into its guard page: save the trap on this CPU's exception
     * stack (struct cpu_record's exception_sp), or on the faulting stack before
     * smp_init sets one. TPIDRRO_EL0 holds x0 meanwhile, and SP_EL0 the
     * faulting SP, which save_frame records as the trap's SP. */
kernel_exception:
    msr     tpidrro_el0, x0
    mov     x0, sp
    msr     sp_el0, x0
    mrs     x0, tpidr_el1
    ldr     x0, [x0, #ARCH_CPU_EXCEPTION_SP]
    cbz     x0, 1f
    mov     sp, x0
1:  mrs     x0, tpidrro_el0
    save_frame
    mov     x0, #1
    b       dispatch

dispatch:
    str     x0, [sp, #272]
    mov     x0, sp
    bl      arm_trap_dispatch
    add     sp, sp, #304
    /* Return from arm_user_run with the kernel's registers. */
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #96
    ret

    /* void arm_user_run(struct user_context *): runs a user slice. */
    .global arm_user_run
    .type arm_user_run, %function
arm_user_run:
    sub     sp, sp, #96
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    mrs     x1, tpidr_el1
    mov     x2, sp
    str     x2, [x1]            /* struct cpu_record's kernel_sp */
    clrex
    ldp     x1, x2, [x0, #256]
    msr     elr_el1, x1
    msr     spsr_el1, x2
    ldr     x1, [x0, #248]
    msr     sp_el0, x1
    ldp     x2, x3, [x0, #16]
    ldp     x4, x5, [x0, #32]
    ldp     x6, x7, [x0, #48]
    ldp     x8, x9, [x0, #64]
    ldp     x10, x11, [x0, #80]
    ldp     x12, x13, [x0, #96]
    ldp     x14, x15, [x0, #112]
    ldp     x16, x17, [x0, #128]
    ldp     x18, x19, [x0, #144]
    ldp     x20, x21, [x0, #160]
    ldp     x22, x23, [x0, #176]
    ldp     x24, x25, [x0, #192]
    ldp     x26, x27, [x0, #208]
    ldp     x28, x29, [x0, #224]
    ldr     x30, [x0, #240]
    ldp     x0, x1, [x0, #0]
    eret
    .size arm_user_run, .-arm_user_run

/* Cooperative kernel-only stack switch, with interrupts masked; kernel C is
 * built with general registers only. The frame matches arch_kernel_frame
 * (arch/arm64/include/arch/kernel_switch.h): x19-x30 upwards from SP. */
    .global kernel_switch
    .type kernel_switch, %function
kernel_switch:
    sub     sp, sp, #96
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    mov     x2, sp
    str     x2, [x0]
    mov     sp, x1
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #96
    ret
    .size kernel_switch, .-kernel_switch

    /* jmp_buf (include/boot/setjmp.h): x19-x30, then SP. */
    .global sos2_boot_setjmp, sos2_boot_longjmp
    .type sos2_boot_setjmp, %function
sos2_boot_setjmp:
    stp     x19, x20, [x0, #0]
    stp     x21, x22, [x0, #16]
    stp     x23, x24, [x0, #32]
    stp     x25, x26, [x0, #48]
    stp     x27, x28, [x0, #64]
    stp     x29, x30, [x0, #80]
    mov     x1, sp
    str     x1, [x0, #96]
    mov     w0, #0
    ret
    .size sos2_boot_setjmp, .-sos2_boot_setjmp
    .type sos2_boot_longjmp, %function
sos2_boot_longjmp:
    ldp     x19, x20, [x0, #0]
    ldp     x21, x22, [x0, #16]
    ldp     x23, x24, [x0, #32]
    ldp     x25, x26, [x0, #48]
    ldp     x27, x28, [x0, #64]
    ldp     x29, x30, [x0, #80]
    ldr     x2, [x0, #96]
    mov     sp, x2
    cmp     w1, #0
    csinc   w0, w1, wzr, ne
    ret
    .size sos2_boot_longjmp, .-sos2_boot_longjmp

    /* User FP/SIMD state and TLS pointer (struct user_runtime). The
     * kernel never uses these registers, so they hold the user's between
     * restore and save. */
    .global user_runtime_init, user_runtime_save, user_runtime_restore
    .type user_runtime_init, %function
user_runtime_init:
    mov     x0, #(3 << 20)      /* CPACR_EL1.FPEN: no FP/SIMD traps */
    msr     cpacr_el1, x0
    isb
    ret
    .type user_runtime_save, %function
user_runtime_save:
    stp     q0, q1, [x0, #0]
    stp     q2, q3, [x0, #32]
    stp     q4, q5, [x0, #64]
    stp     q6, q7, [x0, #96]
    stp     q8, q9, [x0, #128]
    stp     q10, q11, [x0, #160]
    stp     q12, q13, [x0, #192]
    stp     q14, q15, [x0, #224]
    stp     q16, q17, [x0, #256]
    stp     q18, q19, [x0, #288]
    stp     q20, q21, [x0, #320]
    stp     q22, q23, [x0, #352]
    stp     q24, q25, [x0, #384]
    stp     q26, q27, [x0, #416]
    stp     q28, q29, [x0, #448]
    stp     q30, q31, [x0, #480]
    mrs     x1, fpsr
    mrs     x2, fpcr
    str     w1, [x0, #512]
    str     w2, [x0, #516]
    mrs     x1, tpidr_el0
    str     x1, [x0, #520]
    ret
    .type user_runtime_restore, %function
user_runtime_restore:
    ldp     q0, q1, [x0, #0]
    ldp     q2, q3, [x0, #32]
    ldp     q4, q5, [x0, #64]
    ldp     q6, q7, [x0, #96]
    ldp     q8, q9, [x0, #128]
    ldp     q10, q11, [x0, #160]
    ldp     q12, q13, [x0, #192]
    ldp     q14, q15, [x0, #224]
    ldp     q16, q17, [x0, #256]
    ldp     q18, q19, [x0, #288]
    ldp     q20, q21, [x0, #320]
    ldp     q22, q23, [x0, #352]
    ldp     q24, q25, [x0, #384]
    ldp     q26, q27, [x0, #416]
    ldp     q28, q29, [x0, #448]
    ldp     q30, q31, [x0, #480]
    ldr     w1, [x0, #512]
    ldr     w2, [x0, #516]
    msr     fpsr, x1
    msr     fpcr, x2
    ldr     x1, [x0, #520]
    msr     tpidr_el0, x1
    ret
    .section .note.GNU-stack, "", %progbits
