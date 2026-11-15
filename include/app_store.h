#ifndef SOS2_APP_STORE_H
#define SOS2_APP_STORE_H
#include "apps.h"
#include "system_store.h"

/* Native apps installed apart from the system (docs/redmi2.md#apps). A
 * package, <id>.sos2app (tools/package_redmi2.py), is a 512-byte header
 * (struct app_package), the icon and the program. On a phone started from
 * its installed system, the app store holds them after the two system slots
 * of the cache partition: APP_LIMIT positions of two 3 MiB halves, each a
 * 4 KiB header (struct app_store_half) and a package. A position's app is in
 * the half with a valid header and the higher sequence, so an update writes
 * the other half and commits by writing its header last. */
enum {
    APP_PACKAGE_HEADER = 512,
    APP_ICON_BYTES = APP_ICON_SIZE * APP_ICON_SIZE * 4,
    APP_PROGRAM_OFFSET = APP_PACKAGE_HEADER + APP_ICON_BYTES,
    APP_HALF_SECTORS = 6144,
    APP_HALF_HEADER = 4096,
    APP_PACKAGE_MAX = APP_HALF_SECTORS * 512 - APP_HALF_HEADER,
    APP_STORE_START = SYSTEM_SLOT_START + 2 * SYSTEM_SLOT_SECTORS,
    APP_STORE_SECTORS = APP_LIMIT * 2 * APP_HALF_SECTORS
};

_Static_assert((unsigned)APP_STORE_SECTORS == (unsigned)SYSTEM_APPS_SECTORS,
               "the data volume follows the app store");

struct app_package {
    char magic[8];         /* "SOS2APP1" */
    uint32_t version, abi; /* 1, the USER_ABI_VERSION it was built for */
    char id[16], name[16]; /* zero-padded */
    uint32_t icon_offset, icon_size, program_offset, program_size, size;
    uint8_t sha256[32]; /* of the bytes after this header */
    uint32_t crc32;     /* of the 100 bytes above */
};

struct app_store_half {
    char magic[8];                              /* "SOS2APPS" */
    uint32_t version, sequence, size, reserved; /* 1, higher wins, package size, 0 */
    char id[16];
    uint8_t sha256[32]; /* of the package */
    uint32_t crc32;     /* of the 72 bytes above */
};

_Static_assert(sizeof(struct app_package) == 104, "app package header layout");
_Static_assert(sizeof(struct app_store_half) == 76, "app store half layout");

/* A package's header and size checked: magic, version, id, name, the icon
 * and program where they belong, its CRC-32. The ABI and SHA-256 are the
 * caller's to check. */
bool app_package_valid(const struct app_package *, uint32_t size);
/* At boot: the apps installed in the store, into the registry (apps.c). */
void app_store_load(void);
/* On the service worker. The program of an installed app, read whole and
 * checked against its SHA-256 into pages from vm_cpu_alloc, which the caller
 * frees; false if it cannot be read or does not match. */
bool app_store_program(const struct app *, unsigned char **buffer, unsigned *pages,
                       const void **program, uint32_t *size);
/* Install a checked package into the position its app has, or a free one;
 * then its record is the new app's. 0, or a negative USER_E* value with a
 * message. */
int32_t app_store_install(const unsigned char *package, uint32_t size, const char **message);
int32_t app_store_remove(const char *id, const char **message);
#endif
