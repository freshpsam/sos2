#ifndef SOS2_POLL_H
#define SOS2_POLL_H
/* SOS2 descriptors never block: files, the console and user-space devices
 * report every requested event ready, and invalid descriptors POLLNVAL. */
#define POLLIN     0x001
#define POLLPRI    0x002
#define POLLOUT    0x004
#define POLLERR    0x008
#define POLLHUP    0x010
#define POLLNVAL   0x020
#define POLLRDNORM 0x040
#define POLLRDBAND 0x080
#define POLLWRNORM 0x100
#define POLLWRBAND 0x200
typedef unsigned int nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};
#ifdef __cplusplus
extern "C" {
#endif
int poll(struct pollfd *fds, nfds_t count, int timeout);
#ifdef __cplusplus
}
#endif
#endif
