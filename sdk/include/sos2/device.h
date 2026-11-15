#ifndef SOS2_DEVICE_H
#define SOS2_DEVICE_H
#include <stddef.h>
#include <sys/types.h>

/* Devices served in user space. A library registers a path such as
 * /dev/dri/renderD128, normally from a constructor. open() of that path calls
 * the device's open and returns a descriptor; dup() and fcntl(F_DUPFD) share
 * its open description, and release runs after the last descriptor closes.
 * ioctl() and non-anonymous mmap() of the descriptor reach the device, and so
 * does munmap() of addresses the device's mmap returned. Operations other
 * than munmap return -1 (MAP_FAILED for mmap) with errno set on failure. */
struct sos2_device {
    const char *path;
    void *(*open)(int flags);
    void (*release)(void *state);
    int (*ioctl)(void *state, unsigned long request, void *argument);
    void *(*mmap)(void *state, size_t length, int prot, int flags, off_t offset);
    /* Returns 1 when address was one of the device's mappings, else 0. */
    int (*munmap)(void *state, void *address, size_t length);
};
#ifdef __cplusplus
extern "C" {
#endif
/* Returns 0, or -1 with errno EEXIST or ENOMEM. */
int sos2_device_register(const struct sos2_device *device);
/* A new descriptor for state the library made itself (a buffer exported from
 * an open device, say), served by device, which need not be registered.
 * Returns the descriptor, or -1 with errno; release runs only if it succeeds. */
int sos2_device_open_state(const struct sos2_device *device, void *state, int flags);
/* The state behind fd when fd is a descriptor of device; otherwise NULL with
 * errno EBADF (not open) or ENODEV (another kind). The descriptor must stay
 * open while the state is used. */
void *sos2_device_state(int fd, const struct sos2_device *device);
#ifdef __cplusplus
}
#endif
#endif
