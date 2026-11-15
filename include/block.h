#ifndef SOS2_BLOCK_H
#define SOS2_BLOCK_H
#include "fdt.h"
/* Single owned disk: QEMU virtio, the phone's dedicated SOS2 GPT partition or
 * an installed phone's data volume in its cache partition. Transfers use an
 * internal buffer and never expose whole-phone raw writes. */
bool block_init(const struct platform_devices *devices);
uint64_t block_sectors(void);
bool block_read(uint64_t sector, void *data, unsigned sectors);
bool block_write(uint64_t sector, const void *data, unsigned sectors);
bool block_sync(void);
#endif
