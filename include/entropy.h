#ifndef SOS2_ENTROPY_H
#define SOS2_ENTROPY_H
#include "fdt.h"
bool entropy_init(const struct platform_devices *devices);
bool entropy_available(void);
/* Stable diagnostic for the current failure, or NULL while available. */
const char *entropy_error(void);
size_t entropy_read(void *output, size_t size);
/* Bytes at once, outside the service worker, or 0 when only entropy_read
 * can produce them (it may wait). */
size_t entropy_read_now(void *output, size_t size);
#endif
