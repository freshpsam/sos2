#ifndef SOS2_PLATFORM_H
#define SOS2_PLATFORM_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "arch/cpu.h"

static inline uint32_t mmio_read32(uintptr_t p) { return *(volatile uint32_t *)p; }

static inline void mmio_write32(uintptr_t p, uint32_t v) { *(volatile uint32_t *)p = v; }

static inline uint16_t mmio_read16(uintptr_t p) { return *(volatile uint16_t *)p; }

static inline void mmio_write16(uintptr_t p, uint16_t v) { *(volatile uint16_t *)p = v; }

static inline uint8_t mmio_read8(uintptr_t p) { return *(volatile uint8_t *)p; }

static inline void mmio_write8(uintptr_t p, uint8_t v) { *(volatile uint8_t *)p = v; }
struct framebuffer;
void platform_set_framebuffer(struct framebuffer *fb);
uint64_t time_us(void);
/* time_us() as this turn of CPU 0's main loop began (kernel/main.c), so that
 * the loop's periodic checks share one reading of the timer, which takes
 * 0.3 us on the Nexus 5. At most a turn old: for comparing with deadlines in
 * the loop, never for measuring time, nor on other CPUs. Before the loop
 * starts, time_us(). */
uint64_t loop_time_us(void);
void loop_time_update(void);
/* Spins for at least us microseconds. */
void delay_us(uint64_t us);
/* A PSCI or SiP call into the firmware with arguments a and b (and 0 after
 * them), through HVC for CPU_BOOT_HVC (include/fdt.h), else SMC: in
 * arch/arm/psci.S and arch/arm64/psci.S. */
int32_t arm_psci_call(unsigned method, uint32_t function, uintptr_t a, uintptr_t b);
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
