#ifndef SOS2_ARCH_KERNEL_SWITCH_H
#define SOS2_ARCH_KERNEL_SWITCH_H
#include "platform.h"

/* Saves this kernel context's stack pointer in *save and resumes the one at
 * restore (arch/x64/entry.S). */
void kernel_switch(uintptr_t *save, uintptr_t restore);

/* The first stack pointer of a kernel context that kernel_switch starts at
 * entry, on a 16-byte aligned stack ending at top: it pops r15, r14, r13,
 * r12, rbx and rbp, then returns to entry with the stack as a call leaves
 * it. entry never returns. */
static inline uintptr_t arch_kernel_frame(unsigned char *top, void (*entry)(void)) {
    uint64_t *frame = (uint64_t *)top - 8;
    memset(frame, 0, 8 * sizeof(*frame));
    frame[6] = (uintptr_t)entry;
    return (uintptr_t)frame;
}
#endif
