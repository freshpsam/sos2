/* The program entry: sos2_start(mode, pid, options) (start.c), with the stack
 * the kernel provides. ARM32 also has the EABI's thread pointer helper. */
#ifdef __x86_64__
    /* rdi, rsi and rdx hold sos2_start's arguments and r10 the counter's
     * frequency (include/user_abi.h); the stack is as a call leaves it. */
    .text
    .global _start
    .type _start, @function
_start:
    movq    %r10, sos2_counter_frequency(%rip)
    xorl    %ebp, %ebp
    jmp     sos2_start
    .size _start, .-_start
#elif defined(__aarch64__)
    .text
    .balign 4
    .global _start
    .type _start, %function
_start:
    b       sos2_start
    .size _start, .-_start
#else
    .syntax unified
    .arm
    .text
    .align 2
    .global _start
    .type _start, %function
_start:
    b       sos2_start
    .size _start, .-_start
    .global __aeabi_read_tp
    .type __aeabi_read_tp, %function
__aeabi_read_tp:
    mrc     p15, 0, r0, c13, c0, 3
    bx      lr
    .size __aeabi_read_tp, .-__aeabi_read_tp
#endif
    .section .note.GNU-stack, "", %progbits
