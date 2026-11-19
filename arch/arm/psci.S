    .syntax unified
    .arm
    .text
/* int32_t arm_psci_call(unsigned method, uint32_t function, uintptr_t a,
 * uintptr_t b): a PSCI call with a, b and 0 as its arguments (CPU_ON's
 * context ID), through HVC when method is CPU_BOOT_HVC (2), else SMC. */
    .global arm_psci_call
    .type arm_psci_call, %function
arm_psci_call:
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
    .size arm_psci_call, .-arm_psci_call
    .section .note.GNU-stack, "", %progbits
