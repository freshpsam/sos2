#ifndef SOS2_BOOT_SETJMP_H
#define SOS2_BOOT_SETJMP_H
#include <stdint.h>
/* Freestanding decoder and entropy guards; kernel C uses no FP registers. */
typedef uint32_t jmp_buf[10];
int sos2_boot_setjmp(jmp_buf) __attribute__((returns_twice));
_Noreturn void sos2_boot_longjmp(jmp_buf, int);
#define setjmp  sos2_boot_setjmp
#define longjmp sos2_boot_longjmp
#endif
