    .syntax unified
    .arm
    .text
    .align 2
    .global arm_scm_invoke
    .type arm_scm_invoke, %function
arm_scm_invoke:
    push    {r4-r8, lr}
    mov     r8, r0
    ldmia   r8, {r0-r7}
    dsb     sy
    smc     #0
    dsb     sy
    stmia   r8, {r0-r7}
    pop     {r4-r8, pc}
    .size arm_scm_invoke, .-arm_scm_invoke
    .section .note.GNU-stack, "", %progbits
