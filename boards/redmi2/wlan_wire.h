#ifndef SOS2_WLAN_WIRE_H
#define SOS2_WLAN_WIRE_H
#include <stdint.h>
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
#define __packed      __attribute__((packed))
#define ETH_ALEN      6
#define BIT(n)        (1u << (n))
#define GENMASK(h, l) ((~0u << (l)) & (~0u >> (31 - (h))))
#include "../../third_party/wcn36xx/hal.h"
#include "../../third_party/wcn36xx/dxe.h"
#include "../../third_party/wcn36xx/bd.h"
#endif
