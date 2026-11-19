#ifndef SOS2_BOOT_SETJMP_H
#define SOS2_BOOT_SETJMP_H
#include <stdint.h>
/* For the jitter-entropy collector's guards; kernel C uses no FP registers.
 * The callee-saved registers, stack pointer and return address: r4-r11, sp
 * and lr on ARM32, x19-x30 and sp on arm64. */
#ifdef __aarch64__
typedef uint64_t jmp_buf[14];
#else
typedef uint32_t jmp_buf[10];
#endif
int sos2_boot_setjmp(jmp_buf) __attribute__((returns_twice));
_Noreturn void sos2_boot_longjmp(jmp_buf, int);
#define setjmp  sos2_boot_setjmp
#define longjmp sos2_boot_longjmp
#endif
