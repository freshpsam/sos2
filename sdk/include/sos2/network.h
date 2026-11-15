#ifndef SOS2_SDK_NETWORK_H
#define SOS2_SDK_NETWORK_H
#include <stddef.h>
#include <user_net.h>
#ifdef __cplusplus
extern "C" {
#endif
/* All calls are nonblocking; errors return -1 and set errno. connect returns a
 * private handle immediately; poll status until CONNECTED or FAILED. Status
 * error is a negative USER_E* value, not a Newlib errno. Read returns 0 on EOF.
 * Each send/receive accepts at most USER_NET_IO_MAX bytes. EAGAIN means retry
 * after yielding/sleeping; read may return fewer bytes. There is no POSIX fd. */
int sos2_net_info(struct user_net_info *);
int sos2_net_connect(const char *host, unsigned port);
int sos2_net_status(int handle, struct user_net_status *);
int sos2_net_send(int handle, const void *, size_t);
int sos2_net_recv(int handle, void *, size_t);
/* Graceful close when possible, otherwise abort; termination always aborts. */
int sos2_net_close(int handle);
#ifdef __cplusplus
}
#endif
#endif
