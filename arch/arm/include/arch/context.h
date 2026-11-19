#ifndef SOS2_ARCH_CONTEXT_H
#define SOS2_ARCH_CONTEXT_H
#include <stddef.h>
#include <stdint.h>

/* An AArch32 user thread's registers, in the layout arch/arm/process.S saves
 * and restores. CPSR is never supplied by a syscall. Syscalls take their
 * number in r[SYSCALL_NUMBER], arguments in r[0] to r[3], and return in r[0]. */
typedef uint32_t user_reg;
/* A syscall result as wide as a register: a value, an address or USER_E*. */
typedef int32_t user_result;
#define SYSCALL_NUMBER 7

struct arm_context {
    uint32_t r[13], sp, lr, pc, cpsr;
};

struct arm_trap {
    struct arm_context context;
    uint32_t vector, status, address;
};

/* thread_pointer is TPIDRURO, the SDK's TLS pointer; tls is TPIDRURW. */
struct arm_runtime_context {
    uint64_t d[32];
    uint32_t fpscr, tls, thread_pointer, padding;
};

_Static_assert(sizeof(struct arm_trap) == 80, "assembly trap size");
_Static_assert(offsetof(struct arm_context, sp) == 52, "assembly user SP");
_Static_assert(offsetof(struct arm_context, pc) == 60, "assembly PC");
_Static_assert(offsetof(struct arm_context, cpsr) == 64, "assembly CPSR");

/* Designated initializers for a new user thread's context, after any .r:
 * user mode, ARM state, asynchronous aborts and FIQs masked. */
#define USER_CONTEXT_FIELDS(entry, stack) .sp = (stack), .pc = (entry), .cpsr = 0x150

/* Whether the trap interrupted user mode. */
static inline int arch_trap_from_user(const struct arm_trap *trap) {
    return (trap->context.cpsr & 31) == 16;
}
#endif
