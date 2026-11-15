#ifndef SOS2_HANDOFF_H
#define SOS2_HANDOFF_H
#include "fdt.h"
/* NULL on success, otherwise a fixed diagnostic string. No MMIO or writes. */
const char *redmi2_validate_handoff(const struct platform_devices *d, uint64_t kernel_start,
                                    uint64_t kernel_end, uint64_t dtb_address);
#endif
