#ifndef SOS2_SYS_STATVFS_H
#define SOS2_SYS_STATVFS_H
#include <sys/types.h>
#define ST_RDONLY 1
#define ST_NOSUID 2

struct statvfs {
    unsigned long f_bsize, f_frsize;
    fsblkcnt_t f_blocks, f_bfree, f_bavail;
    fsfilcnt_t f_files, f_ffree, f_favail;
    unsigned long f_fsid, f_flag, f_namemax;
};
#ifdef __cplusplus
extern "C" {
#endif
int statvfs(const char *, struct statvfs *);
#ifdef __cplusplus
}
#endif
#endif
