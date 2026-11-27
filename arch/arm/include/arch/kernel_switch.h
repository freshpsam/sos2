#ifndef SOS2_ARCH_KERNEL_SWITCH_H
#define SOS2_ARCH_KERNEL_SWITCH_H
#include "platform.h"

/* Saves this kernel context's stack pointer in *save and resumes the one at
 * restore (arch/arm/process.S). */
void kernel_switch(uintptr_t *save, uintptr_t restore);

/* The first stack pointer of a kernel context that kernel_switch starts at
 * entry, on a stack ending at top: it pops r4-r11, r12 (alignment) and pc. */
static inline uintptr_t arch_kernel_frame(unsigned char *top, void (*entry)(void)) {
    uint32_t *frame = (uint32_t *)top - 10;
    memset(frame, 0, 10 * sizeof(*frame));
    frame[9] = (uintptr_t)entry;
    return (uintptr_t)frame;
}
#endif
