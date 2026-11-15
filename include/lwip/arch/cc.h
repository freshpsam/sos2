#ifndef SOS2_LWIP_CC_H
#define SOS2_LWIP_CC_H
#include <stdint.h>
uint32_t network_random(void);
void network_assert(void);
#define LWIP_RAND() network_random()
#define LWIP_PLATFORM_DIAG(x) do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) network_assert()
#define LWIP_NO_STDINT_H        0
#define LWIP_NO_INTTYPES_H      1
#define LWIP_NO_CTYPE_H         1
#define X8_F                    "02x"
#define U16_F                   "u"
#define S16_F                   "d"
#define X16_F                   "x"
#define U32_F                   "u"
#define S32_F                   "d"
#define X32_F                   "x"
#define SZT_F                   "u"
#define LWIP_NO_LIMITS_H        0
#endif
