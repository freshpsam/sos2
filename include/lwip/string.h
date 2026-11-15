#ifndef SOS2_LWIP_STRING_H
#define SOS2_LWIP_STRING_H
/* The stack's freestanding C boundary uses the kernel's byte/string routines. */
#include "platform.h"
int strncmp(const char *, const char *, size_t);
char *strstr(const char *, const char *);
#endif
