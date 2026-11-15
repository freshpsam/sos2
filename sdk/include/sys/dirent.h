#ifndef SOS2_SYS_DIRENT_H
#define SOS2_SYS_DIRENT_H
#include <sys/types.h>
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[128];
};
typedef struct sos2_directory DIR;
#ifdef __cplusplus
extern "C" {
#endif
DIR *opendir(const char *);
struct dirent *readdir(DIR *);
int closedir(DIR *);
#ifdef __cplusplus
}
#endif
#endif
