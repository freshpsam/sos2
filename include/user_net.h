#ifndef SOS2_USER_NET_H
#define SOS2_USER_NET_H
#include <stdint.h>
/* Native nonblocking TCP handles, private to each process. Addresses below are
 * four network-order bytes. No kernel pointers or lwIP structures cross the ABI. */
#define USER_NET_SOCKETS  8u
#define USER_NET_HOST_MAX 253u
#define USER_NET_IO_MAX   4096u
#define NET_DEVICE        1u
#define NET_LINK          2u
#define NET_ADDRESS       4u
#define NET_CONNECTING    1u
#define NET_CONNECTED     2u
#define NET_FAILED        3u
#define NET_READ_EOF      1u

struct user_net_info {
    uint32_t flags, mtu, rx_packets, tx_packets;
    uint8_t address[4], mask[4], gateway[4], dns[4], mac[6], reserved[2];
};

struct user_net_status {
    uint32_t state, flags, readable, writable;
    int32_t error;
};
#endif
