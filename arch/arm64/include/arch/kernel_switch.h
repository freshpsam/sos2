#ifndef SOS2_ARCH_KERNEL_SWITCH_H
#define SOS2_ARCH_KERNEL_SWITCH_H
#include "platform.h"

/* Saves this kernel context's stack pointer in *save and resumes the one at
 * restore (arch/arm64/entry.S). */
void arm_kernel_switch(uintptr_t *save, uintptr_t restore);

/* The first stack pointer of a kernel context that arm_kernel_switch starts at
 * entry, on a 16-byte aligned stack ending at top: it pops x19-x30 and
 * returns to x30. */
static inline uintptr_t arch_kernel_frame(unsigned char *top, void (*entry)(void)) {
    uint64_t *frame = (uint64_t *)top - 12;
    memset(frame, 0, 12 * sizeof(*frame));
    frame[11] = (uintptr_t)entry;
    return (uintptr_t)frame;
}
#endif
