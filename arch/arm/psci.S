    .syntax unified
    .arm
    .text
    .global arm_psci_cpu_on
    .type arm_psci_cpu_on, %function
arm_psci_cpu_on:
    push    {r4-r8, lr}
    mov     r4, r0
    mov     r0, r1
    mov     r1, r2
    mov     r2, r3
    mov     r3, #0
    cmp     r4, #2
    bne     1f
    hvc     #0
    b       2f
1:  smc     #0
2:  pop     {r4-r8, pc}
    .size arm_psci_cpu_on, .-arm_psci_cpu_on
    .section .note.GNU-stack, "", %progbits
