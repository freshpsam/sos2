#include "internal.h"
#include <sos2/network.h>
#include <errno.h>

int sos2_net_info(struct user_net_info *info) {
    return sos2_result(call(SYS_NET_INFO, sos2_ptr(info), sizeof(*info)));
}

int sos2_net_connect(const char *host, unsigned port) {
    return sos2_result(call(SYS_NET_CONNECT, sos2_ptr(host), port));
}

int sos2_net_status(int handle, struct user_net_status *status) {
    return sos2_result(call3(SYS_NET_STATUS, handle, sos2_ptr(status), sizeof(*status)));
}

int sos2_net_send(int handle, const void *data, size_t size) {
    if (size > USER_NET_IO_MAX) {
        errno = EINVAL;
        return -1;
    }
    return sos2_result(call3(SYS_NET_SEND, handle, sos2_ptr(data), (unsigned)size));
}

int sos2_net_recv(int handle, void *data, size_t size) {
    if (size > USER_NET_IO_MAX) {
        errno = EINVAL;
        return -1;
    }
    return sos2_result(call3(SYS_NET_RECV, handle, sos2_ptr(data), (unsigned)size));
}

int sos2_net_close(int handle) { return sos2_result(call(SYS_NET_CLOSE, handle, 0)); }
