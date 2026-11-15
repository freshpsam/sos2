#ifndef SOS2_RUNTIME_SYSCALL_H
#define SOS2_RUNTIME_SYSCALL_H
#include <stdint.h>
#include "user_abi.h"

static inline int32_t call4(unsigned number, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    register uint32_t r0 __asm__("r0") = a;
    register uint32_t r1 __asm__("r1") = b;
    register uint32_t r2 __asm__("r2") = c;
    register uint32_t r3 __asm__("r3") = d;
    register uint32_t r7 __asm__("r7") = number;
    __asm__ volatile("svc #0"
                     : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r7)
                     :
                     : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t call3(unsigned number, uint32_t a, uint32_t b, uint32_t c) {
    return call4(number, a, b, c, 0);
}

static inline int32_t call(unsigned number, uint32_t a, uint32_t b) {
    return call3(number, a, b, 0);
}

static _Noreturn inline void finish(unsigned status) {
    call(SYS_EXIT, status, 0);
    for (;;) { }
}
#endif
