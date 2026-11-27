#ifndef SOS2_ARCH_CONTEXT_H
#define SOS2_ARCH_CONTEXT_H
#include <stddef.h>
#include <stdint.h>

/* An x64 user thread's registers, in the layout arch/x64/entry.S saves
 * and restores. SOS2's syscalls are its own ABI, used only by the SDK
 * (sdk/runtime/syscall.h): the number in rax (r[SYSCALL_NUMBER]), arguments
 * in rdi, rsi, rdx and r10 (r[0] to r[3]) and the result in rdi (r[0]), so
 * that the shared dispatcher reads and writes the same slots as on ARM.
 * SYSCALL itself overwrites rcx and r11 with the return address and flags. */
typedef uint64_t user_reg;
/* A syscall result as wide as a register: a value, an address or USER_E*. */
typedef int64_t user_result;
#define SYSCALL_NUMBER 6

/* r: rdi, rsi, rdx, r10, r8, r9, rax, rbx, rcx, rbp, r11, r12, r13, r14 and
 * r15; then rsp, rip and rflags. */
struct user_context {
    uint64_t r[15], sp, pc, flags;
};

/* vector uses arch/arm's codes, which the shared kernel reads (1 other
 * fault, 2 syscall, 3 instruction fetch fault, 4 data fault, 6 the
 * scheduler's timer); status is the x86 vector above its error code
 * (vector << 32 | code), address CR2 and cs the trapped code segment. */
struct cpu_trap {
    struct user_context context;
    uint64_t vector, status, address, cs;
};

/* FXSAVE's SSE and x87 state, and the FS base: the SDK's TLS pointer, which
 * only SYS_THREAD_TLS sets (user code has no FSGSBASE). A record never saved
 * (valid 0, as new threads' are) restores the reset state: MXCSR 0x1F80, x87
 * control word 0x37F, every register zero, so nothing passes between
 * threads (arch/x64/entry.S). */
struct user_runtime {
    _Alignas(16) unsigned char fxsave[512];
    uint64_t thread_pointer, valid;
};

_Static_assert(sizeof(struct user_context) == 144, "assembly context size");
_Static_assert(offsetof(struct user_context, sp) == 120, "assembly user SP");
_Static_assert(offsetof(struct user_context, pc) == 128, "assembly PC");
_Static_assert(offsetof(struct user_context, flags) == 136, "assembly flags");
_Static_assert(sizeof(struct cpu_trap) == 176, "assembly trap size");
_Static_assert(sizeof(struct user_runtime) == 528, "assembly runtime size");

/* Designated initializers for a new user thread's context, after any .r:
 * interrupts on, and the stack as a call leaves it (8 below a 16-byte
 * boundary), which compiled code's aligned SSE stack slots rely on. */
#define USER_CONTEXT_FIELDS(entry, stack) .sp = (stack) - 8, .pc = (entry), .flags = 0x202

/* A user entry point: any address of the canonical lower half. */
static inline int arch_user_entry_valid(user_reg entry) { return entry < (1ull << 47); }

/* Whether the trap interrupted user mode (CPL 3). */
static inline int arch_trap_from_user(const struct cpu_trap *trap) { return (trap->cs & 3) == 3; }
#endif
