#ifndef SOS2_NETWORK_TIME_H
#define SOS2_NETWORK_TIME_H
#include "platform.h"
/* Called only by the kernel network poller, after DHCP and entropy setup. */
void network_time_init(void);
/* now: time_us() at the poll. */
void network_time_poll(bool addressed, uint64_t now);
#endif
