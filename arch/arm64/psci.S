    .text
/* int32_t arm_psci_call(unsigned method, uint32_t function, uintptr_t a,
 * uintptr_t b): a PSCI or SiP call with a, b and 0 as its arguments (CPU_ON's
 * context ID), through HVC when method is CPU_BOOT_HVC (2), else SMC. SMCCC
 * lets the call change x0-x17, which the AAPCS64 caller does not expect
 * preserved. */
    .global arm_psci_call
    .type arm_psci_call, %function
arm_psci_call:
    mov     w4, w0
    mov     w0, w1
    mov     x1, x2
    mov     x2, x3
    mov     x3, #0
    cmp     w4, #2
    b.ne    1f
    hvc     #0
    ret
1:  smc     #0
    ret
    .size arm_psci_call, .-arm_psci_call
    .section .note.GNU-stack, "", %progbits
