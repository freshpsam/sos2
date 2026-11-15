#ifndef SOS2_SYS_IOCCOM_H
#define SOS2_SYS_IOCCOM_H
/* ioctl request numbers, BSD style (DRM headers include this header on
 * systems other than Linux). The encoding matches Linux's: direction in bits
 * 30-31, argument size in 16-29, group in 8-15, number in 0-7. Requests
 * reach user-space devices only; there is no ioctl system call. */
#define IOCPARM_SHIFT  16
#define IOCPARM_MASK   0x3fffu
#define IOCPARM_LEN(x) (((x) >> IOCPARM_SHIFT) & IOCPARM_MASK)
#define IOCBASECMD(x)  ((x) & ~(IOCPARM_MASK << IOCPARM_SHIFT))
#define IOCGROUP(x)    (((x) >> 8) & 0xffu)
#define IOC_VOID       0x00000000u
#define IOC_OUT        0x80000000u
#define IOC_IN         0x40000000u
#define IOC_INOUT      (IOC_IN | IOC_OUT)
#define IOC_DIRMASK    (IOC_IN | IOC_OUT)
#define _IOC(inout, group, num, len) \
    ((unsigned long)((inout) | (((len) & IOCPARM_MASK) << IOCPARM_SHIFT) | ((group) << 8) | (num)))
#define _IO(g, n)      _IOC(IOC_VOID, (g), (n), 0)
#define _IOR(g, n, t)  _IOC(IOC_OUT, (g), (n), sizeof(t))
#define _IOW(g, n, t)  _IOC(IOC_IN, (g), (n), sizeof(t))
#define _IOWR(g, n, t) _IOC(IOC_INOUT, (g), (n), sizeof(t))
#endif
