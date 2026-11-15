#ifndef SOS2_WIFI_PORT_H
#define SOS2_WIFI_PORT_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
/* One serialized radio service owns the bounded authentication arena/timers. */
int wifi_os_snprintf(char *, size_t, const char *, ...);
void wifi_port_bind(int (*random)(void *, unsigned char *, size_t), void *, uint64_t);
void wifi_port_tick(uint64_t);
void wifi_port_reset(void);
void wifi_port_set_yield(void (*)(void *), void *);
void wifi_port_yield(void);
bool wifi_port_failed(void);
void *wifi_os_malloc(size_t);
void *wifi_os_zalloc(size_t);
void *wifi_os_realloc(void *, size_t);
void wifi_os_free(void *);
char *wifi_os_strdup(const char *);
void *wifi_crypto_calloc(size_t, size_t);
void wifi_crypto_wipe(void *, size_t);
int wifi_crypto_random(void *, unsigned char *, size_t);
_Noreturn void wifi_os_abort(void);
#endif
