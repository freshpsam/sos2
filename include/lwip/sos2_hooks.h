#ifndef SOS2_LWIP_HOOKS_H
#define SOS2_LWIP_HOOKS_H
struct dhcp;
struct dhcp_msg;
void network_dhcp_prepare(struct dhcp *, struct dhcp_msg *, unsigned type);
#define LWIP_HOOK_DHCP_APPEND_OPTIONS(netif, dhcp, state, msg, type, length) \
    network_dhcp_prepare(dhcp, msg, type)
#endif
