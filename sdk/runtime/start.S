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
    .section .note.GNU-stack, "", %progbits
