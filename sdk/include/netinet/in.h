#ifndef SOS2_NETINET_IN_H
#define SOS2_NETINET_IN_H
#include <stdint.h>
/* Address values for parsers; these declarations do not provide sockets. */
#define AF_UNSPEC        0
#define AF_INET          2
#define AF_INET6         10
#define INADDR_ANY       ((uint32_t)0)
#define INADDR_LOOPBACK  ((uint32_t)0x7f000001)
#define INADDR_BROADCAST ((uint32_t)0xffffffff)
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct in_addr {
    in_addr_t s_addr;
};

struct in6_addr {
    uint8_t s6_addr[16];
};
#ifdef __cplusplus
extern "C" {
#endif
uint32_t htonl(uint32_t);
uint32_t ntohl(uint32_t);
uint16_t htons(uint16_t);
uint16_t ntohs(uint16_t);
#ifdef __cplusplus
}
#endif
#endif
