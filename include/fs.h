#ifndef SOS2_FS_H
#define SOS2_FS_H
#include "vm.h"

/* A process's /data and /tmp are its profile's directories on the data and
 * temporary volumes: /profiles/browser for every web page, /profiles/updater
 * for the system's updater, /profiles/apps/<id> for an app. Handles remain
 * bounded per process. */
struct fs_handle {
    uint32_t kind, flags, offset;
    void *object;
    char path[128];
};
struct fs_pool; /* Per-process open-file tables, allocated by fs_space_init. */
#define FS_PROFILE_MAX 32u

struct fs_space {
    char profile[FS_PROFILE_MAX];
    struct fs_pool *pool;
    struct fs_handle handles[USER_FILE_HANDLES];
};

void fs_init(const struct platform_devices *devices);
bool fs_persistent_ready(void);
/* A RAM session: private /data and settings live only in RAM. Booted with
 * sos2.storage=ram, no data disk is initialized, read, written or flushed; an
 * installed phone's system store (include/system_store.h) is separate. An
 * installed system (sos2.storage=installed) whose data volume is unavailable
 * runs as one too. */
bool fs_session_only(void);

/* Desktop-only settings. Call on the serialized I/O worker; never a user path. */
enum fs_setting { FS_SETTING_WIFI, FS_SETTING_CLOCK, FS_SETTING_COUNT };

int32_t fs_settings_read(enum fs_setting, void *, unsigned capacity);
int32_t fs_settings_write(enum fs_setting, const void *, unsigned size);
/* profile: "browser", "updater" or "apps/<id>", below /profiles; created as
 * needed. */
bool fs_space_init(struct fs_space *space, const char *profile);
/* Also releases the process's SYS_FILE_LOCK locks. */
void fs_space_close(struct fs_space *space);
/* Delete a profile's /data and /tmp, as when its app is removed; on the
 * serialized service worker, with none of the profile's processes running. */
bool fs_profile_remove(const char *profile);
/* Answered directly on the syscall path; no littlefs access. */
int32_t fs_seek(struct fs_space *files, uint32_t handle, uint32_t offset);
int32_t fs_call(struct fs_space *files, struct vm_space *vm, uint32_t number, uint32_t a,
                uint32_t b, uint32_t c, uint32_t d);
#endif
