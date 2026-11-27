#ifndef SOS2_RUNTIME_SYSCALL_H
#define SOS2_RUNTIME_SYSCALL_H
#include <stdint.h>
#include "user_abi.h"

/* Syscall arguments and results are register-wide: a result is a value, an
 * address or a negative USER_E* error (include/user_abi.h). */
#ifdef __aarch64__
typedef uintptr_t sos2_word;
typedef intptr_t sos2_status;

/* svc #0: the number in x8, arguments in x0-x3, the result in x0. */
static inline sos2_status call4(unsigned number, sos2_word a, sos2_word b, sos2_word c,
                                sos2_word d) {
    register sos2_word x0 __asm__("x0") = a;
    register sos2_word x1 __asm__("x1") = b;
    register sos2_word x2 __asm__("x2") = c;
    register sos2_word x3 __asm__("x3") = d;
    register sos2_word x8 __asm__("x8") = number;
    __asm__ volatile("svc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x8)
                     :
                     : "memory", "cc");
    return (sos2_status)x0;
}
#elif defined(__x86_64__)
typedef uintptr_t sos2_word;
typedef intptr_t sos2_status;

/* syscall: the number in rax, arguments in rdi, rsi, rdx and r10, the result
 * in rdi (SOS2's own convention: arch/x64/include/arch/context.h). SYSCALL
 * itself overwrites rcx and r11. */
static inline sos2_status call4(unsigned number, sos2_word a, sos2_word b, sos2_word c,
                                sos2_word d) {
    register sos2_word r10 __asm__("r10") = d;
    sos2_word rax = number;
    __asm__ volatile("syscall"
                     : "+D"(a), "+S"(b), "+d"(c), "+r"(r10), "+a"(rax)
                     :
                     : "rcx", "r11", "memory", "cc");
    return (sos2_status)a;
}
#else
typedef uint32_t sos2_word;
typedef int32_t sos2_status;

/* svc #0: the number in r7, arguments in r0-r3, the result in r0. */
static inline sos2_status call4(unsigned number, sos2_word a, sos2_word b, sos2_word c,
                                sos2_word d) {
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
#endif

static inline sos2_status call3(unsigned number, sos2_word a, sos2_word b, sos2_word c) {
    return call4(number, a, b, c, 0);
}

static inline sos2_status call(unsigned number, sos2_word a, sos2_word b) {
    return call3(number, a, b, 0);
}

static _Noreturn inline void finish(unsigned status) {
    call(SYS_EXIT, status, 0);
    for (;;) { }
}
#endif
