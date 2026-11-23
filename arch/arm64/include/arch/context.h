#ifndef SOS2_ARCH_CONTEXT_H
#define SOS2_ARCH_CONTEXT_H
#include <stddef.h>
#include <stdint.h>

/* An AArch64 user thread's registers, in the layout arch/arm64/entry.S saves
 * and restores: x0-x30, SP_EL0, ELR_EL1 (pc) and SPSR_EL1 (pstate), which a
 * syscall never supplies. Syscalls take their number in r[SYSCALL_NUMBER]
 * (x8), arguments in r[0] to r[5], and return in r[0]. */
typedef uint64_t user_reg;
/* A syscall result as wide as a register: a value, an address or USER_E*. */
typedef int64_t user_result;
#define SYSCALL_NUMBER 8

struct arm_context {
    uint64_t r[31], sp, pc, pstate;
};

/* vector uses arch/arm's codes (2 SVC, 6 IRQ, others faults); status is
 * ESR_EL1 and address FAR_EL1. A kernel exception's context.sp is the kernel
 * SP that faulted (arch/arm64/entry.S), not SP_EL0. */
struct arm_trap {
    struct arm_context context;
    uint64_t vector, status, address, padding;
};

/* The SIMD and floating-point registers, FPSR and FPCR, and TPIDR_EL0: the
 * SDK's TLS pointer, which user code may also change. */
struct arm_runtime_context {
    uint64_t v[64];
    uint32_t fpsr, fpcr;
    uint64_t thread_pointer;
};

_Static_assert(sizeof(struct arm_trap) == 304, "assembly trap size");
_Static_assert(offsetof(struct arm_context, sp) == 248, "assembly user SP");
_Static_assert(offsetof(struct arm_context, pc) == 256, "assembly PC");
_Static_assert(offsetof(struct arm_context, pstate) == 264, "assembly PSTATE");
_Static_assert(sizeof(struct arm_runtime_context) == 528, "assembly runtime size");

/* Designated initializers for a new user thread's context, after any .r:
 * EL0t, AArch64, IRQs unmasked; SError and FIQ stay masked, as ARM32's user CPSR
 * 0x150 masks them. */
#define USER_CONTEXT_FIELDS(entry, stack) .sp = (stack), .pc = (entry), .pstate = 0x140

/* A user entry point: an instruction's word. */
static inline int arch_user_entry_valid(user_reg entry) { return !(entry & 3); }

/* Whether the trap interrupted EL0 in AArch64 (EL0t). */
static inline int arch_trap_from_user(const struct arm_trap *trap) {
    return (trap->context.pstate & 0x1f) == 0;
}
#endif
