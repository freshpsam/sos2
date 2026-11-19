    .syntax unified
    .arm
    .fpu neon-vfpv4
    .text
    .balign 32
    .global arm_process_vectors
arm_process_vectors:
    b       trap_reset
    b       trap_undefined
    b       trap_svc
    b       trap_prefetch
    b       trap_data
    b       trap_reserved
    b       trap_irq
    b       trap_fiq

/* Kernel services are serialized on the boot CPU. A user slice is a bounded
 * call from SVC mode; every trap returns to that saved kernel continuation.
 * No kernel call blocks with a process-specific kernel stack outstanding.
 * User r0-r12, banked SP/LR, PC and CPSR are saved on an 8-byte-aligned frame.
 * IRQ/ABT/UND have separate guarded stacks; SVC uses the saved kernel stack.
 */
    .macro trap name, number, correction=0
\name:
    cpsid   aif
    sub     lr, lr, #\correction
    sub     sp, sp, #80
    stmia   sp, {r0-r12}
    add     r0, sp, #52
    stmia   r0, {sp, lr}^
    nop
    str     lr, [sp, #60]
    mrs     r0, spsr
    str     r0, [sp, #64]
    mov     r0, #\number
    str     r0, [sp, #68]
    mov     r0, #0
    mov     r1, #0
    .if \number == 3
        mrc     p15, 0, r0, c5, c0, 1
        mrc     p15, 0, r1, c6, c0, 2
    .elseif \number == 4
        mrc     p15, 0, r0, c5, c0, 0
        mrc     p15, 0, r1, c6, c0, 0
    .endif
    str     r0, [sp, #72]
    str     r1, [sp, #76]
    mov     r0, sp
    bl      arm_trap_dispatch
    cmp     r0, #0
    bne     1f
    add     sp, sp, #80
    cps     #0x13
    mrc     p15, 0, sp, c13, c0, 4
    ldr     sp, [sp]
    add     sp, sp, #4
    pop     {r4-r11, pc}
    /* Handled on this CPU: resume the user context from the frame. */
1:  ldr     r0, [sp, #64]
    msr     spsr_cxsf, r0
    add     r0, sp, #52
    ldmia   r0, {sp, lr}^
    nop
    ldr     lr, [sp, #60]
    ldmia   sp, {r0-r12}
    add     sp, sp, #80
    clrex
    movs    pc, lr
    .endm
    trap    trap_reset, 0
    trap    trap_undefined, 1, 4
    trap    trap_svc, 2
    trap    trap_prefetch, 3, 4
    trap    trap_data, 4, 8
    trap    trap_reserved, 5
    trap    trap_irq, 6, 4
/* FIQ remains masked and its banked r8-r12 are never used for user execution. */
    trap    trap_fiq, 7, 4

    .global arm_user_run
    .type arm_user_run, %function
arm_user_run:
    clrex
    push    {r4-r11, lr}
    sub     sp, sp, #4
    mrc     p15, 0, r1, c13, c0, 4
    str     sp, [r1]
    mov     lr, r0
    add     r1, lr, #52
    ldmia   r1, {sp, lr}^
    nop
    ldr     r1, [lr, #64]
    msr     spsr_cxsf, r1
    ldmia   lr, {r0-r12}
    ldr     lr, [lr, #60]
    movs    pc, lr
    .size arm_user_run, .-arm_user_run

/* Cooperative kernel-only stack switch. Both callers run in SVC mode with
 * interrupts masked; kernel C is built with general registers only. */
    .global arm_kernel_switch
    .type arm_kernel_switch, %function
arm_kernel_switch:
    push    {r4-r12, lr}
    str     sp, [r0]
    mov     sp, r1
    pop     {r4-r12, pc}
    .size arm_kernel_switch, .-arm_kernel_switch

    .global sos2_boot_setjmp, sos2_boot_longjmp
    .type sos2_boot_setjmp, %function
sos2_boot_setjmp:
    stmia   r0, {r4-r11, sp, lr}
    mov     r0, #0
    bx      lr
    .size sos2_boot_setjmp, .-sos2_boot_setjmp
    .type sos2_boot_longjmp, %function
sos2_boot_longjmp:
    ldmia   r0, {r4-r11, sp, lr}
    cmp     r1, #0
    moveq   r1, #1
    mov     r0, r1
    bx      lr
    .size sos2_boot_longjmp, .-sos2_boot_longjmp

    .global arm_runtime_init, arm_runtime_save, arm_runtime_restore
    .type arm_runtime_init, %function
arm_runtime_init:
    mov     r0, #0x40000000
    vmsr    fpexc, r0
    bx      lr
    .type arm_runtime_save, %function
arm_runtime_save:
    vstmia  r0!, {d0-d15}
    vstmia  r0!, {d16-d31}
    vmrs    r1, fpscr
    str     r1, [r0]
    mrc     p15, 0, r1, c13, c0, 2
    str     r1, [r0, #4]
    mrc     p15, 0, r1, c13, c0, 3
    str     r1, [r0, #8]
    bx      lr
    .type arm_runtime_restore, %function
arm_runtime_restore:
    vldmia  r0!, {d0-d15}
    vldmia  r0!, {d16-d31}
    ldr     r1, [r0]
    vmsr    fpscr, r1
    ldr     r1, [r0, #4]
    mcr     p15, 0, r1, c13, c0, 2
    ldr     r1, [r0, #8]
    mcr     p15, 0, r1, c13, c0, 3
    isb
    bx      lr
    .section .note.GNU-stack, "", %progbits
