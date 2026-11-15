#ifndef SOS2_KERNEL_IO_H
#define SOS2_KERNEL_IO_H
#include <stdbool.h>
/* One serialized service worker for filesystem, firmware and entropy calls, cooperatively
 * resumed from the UI loop. Boot-time calls remain synchronous. */
extern unsigned char kernel_io_stack[];
bool kernel_io_busy(void);
void kernel_io_start(void (*work)(void *), void *argument);
void kernel_io_step(void);
void kernel_io_wait(void);
#endif
