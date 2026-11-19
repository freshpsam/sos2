#ifndef SOS2_HOSTAP_CONFIG_H
#define SOS2_HOSTAP_CONFIG_H
#include "wifi/port.h"
#define CONFIG_SOS2            1
#define CONFIG_SAE             1
#define CONFIG_SHA384          1
#define CONFIG_SHA512          1
#define CONFIG_CRYPTO_INTERNAL 1
#define CONFIG_NO_TKIP         1
#define CONFIG_NO_RC4          1
#define CONFIG_NO_RANDOM_POOL  1
#define CONFIG_NO_STDOUT_DEBUG 1
#define CONFIG_NO_WPA_MSG      1
#define os_malloc              wifi_os_malloc
#define os_zalloc              wifi_os_zalloc
#define os_realloc             wifi_os_realloc
#define os_free                wifi_os_free
#define os_strdup              wifi_os_strdup
#define os_snprintf            wifi_os_snprintf
#define abort                  wifi_os_abort
#define __BYTE_ORDER           1234
#define __LITTLE_ENDIAN        1234
#define __BIG_ENDIAN           4321
#define bswap_16               __builtin_bswap16
#define bswap_32               __builtin_bswap32
#define bswap_64               __builtin_bswap64
#endif
