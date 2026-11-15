#ifndef SOS2_PLATFORM_H
#define SOS2_PLATFORM_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

static inline void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static inline uint32_t mmio_read32(uintptr_t p) { return *(volatile uint32_t *)p; }

static inline void mmio_write32(uintptr_t p, uint32_t v) { *(volatile uint32_t *)p = v; }
struct framebuffer;
void platform_set_framebuffer(struct framebuffer *fb);
uint64_t time_us(void);
_Noreturn void panic(const char *message);
void *memset(void *dst, int value, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
void *memchr(const void *data, int byte, size_t size);
char *strchr(const char *s, int c);
char *strcpy(char *out, const char *s);
/* Text building: both write a terminating NUL and return a pointer to it;
 * decimal writes at most 10 digits. */
char *stpcpy(char *out, const char *s);
char *decimal(char *out, unsigned value);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
#endif
