#ifndef SOS2_SYS_IOCTL_H
#define SOS2_SYS_IOCTL_H
#include <sys/ioccom.h>
/* ioctl reaches devices served in user space (sos2/device.h); any other
 * descriptor fails with ENOTTY. */
#ifdef __cplusplus
extern "C" {
#endif
int ioctl(int fd, unsigned long request, ...);
#ifdef __cplusplus
}
#endif
#endif
