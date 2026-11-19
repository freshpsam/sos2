#ifndef SOS2_WIFI_FREESTANDING_H
#define SOS2_WIFI_FREESTANDING_H
#include "wifi/port.h"
/* Declarations for unused upstream helpers. File/number parsing functions
 * are discarded at link time; the kernel does not provide a POSIX runtime. */
typedef struct wifi_unused_file FILE;
long strtol(const char *, char **, int);
char *strrchr(const char *, int);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
int vsnprintf(char *, size_t, const char *, __builtin_va_list);

static inline int abs(int n) { return n < 0 ? -n : n; }

#define free wifi_os_free
#endif
