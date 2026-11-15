#ifndef SOS2_NETWORK_TIME_H
#define SOS2_NETWORK_TIME_H
#include "platform.h"
/* Called only by the kernel network poller, after DHCP and entropy setup. */
void network_time_init(const char *bootargs);
void network_time_poll(bool addressed);
#endif
