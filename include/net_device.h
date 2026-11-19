#ifndef SOS2_NET_DEVICE_H
#define SOS2_NET_DEVICE_H
#include "fdt.h"
#define NET_FRAME_MAX 1514u

/* Bounded Ethernet interface. A Wi-Fi backend can present associated Ethernet
 * frames here without changing TCP, DNS or the native process API. */
struct net_device {
    bool (*online)(void);
    bool (*link)(void);
    void (*poll)(void (*receive)(const void *, unsigned));
    bool (*send)(const void *, unsigned);
    void (*stop)(void);
    uint8_t mac[6];
    uint32_t rx_packets, tx_packets;
};

bool virtio_net_init(const struct platform_devices *, struct net_device *);
#endif
