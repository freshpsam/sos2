#ifndef SOS2_BOOT_ERRNO_H
#define SOS2_BOOT_ERRNO_H
/* The protected kernel has no errno. third_party/lzma's 7zTypes.h includes
 * this header and names these constants in macros the decoder never uses. */
#define ENOENT 2
#define ENOMEM 12
#define EEXIST 17
#define EINVAL 22
#define ENOSPC 28
#endif
