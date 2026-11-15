#ifndef SOS2_LWIP_STDIO_H
#define SOS2_LWIP_STDIO_H
#include <stddef.h>
/* Only lwIP's optional overflow diagnostic references this. It is disabled;
 * enabling it requires supplying a formatter, not linking a host C library. */
int snprintf(char *, size_t, const char *, ...);
#endif
