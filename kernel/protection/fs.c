#include "fs.h"
#include "block.h"
#include "runtime_files.h"
#include "platform.h"
#include "page_alloc.h"
#include "kernel_io.h"
#include "lfs.h"
#include "system_store.h"

enum {
    BLOCK = 4096,
    CACHE = 512,
    TEMP_BYTES = 1024 * 1024,
    RESERVE_BYTES = 64 * 1024,
    INTERNAL_PATH = 160,
    RESOURCE = 1,
    RESOURCE_DIR = 2,
    DISK_FILE = 3,
    DISK_DIR = 4
};

struct volume {
    lfs_t fs;
    struct lfs_config config;
    unsigned char *memory;
    bool mounted;
    unsigned char read_cache[CACHE], write_cache[CACHE], lookahead[128];
};

/* A lost inode could not be reopened after a rename: its littlefs file is
 * closed and every operation except close fails. */
struct inode {
    struct volume *volume;
    unsigned refs;
    bool lost;
    char path[INTERNAL_PATH];
    lfs_file_t file;
    struct lfs_file_config config;
    unsigned char cache[CACHE];
};

struct directory {
    struct volume *volume;
    lfs_dir_t dir;
    bool used;
};

/* Open-file tables belong to each process, even when pages share the browser
 * profile: a fixed global table would let a few pages exhaust it for all. */
struct fs_pool {
    struct inode inodes[USER_FILE_HANDLES];
    struct directory directories[USER_FILE_HANDLES];
    struct fs_pool *next; /* Every live process's tables, for path changes. */
};
static struct fs_pool *pools;
#define POOL_PAGES ((unsigned)((sizeof(struct fs_pool) + PAGE_SIZE - 1) / PAGE_SIZE))

/* One data volume and one temporary volume. The data volume holds kernel
 * settings, and both hold one directory per profile, which is that profile's
 * /data or /tmp:   /settings/{wifi,clock,reserve}
 *                  /profiles/browser/ (every web page), /profiles/apps/<id>/
 * Paths are canonical and cannot climb out of a profile's directory, so
 * nothing inside a profile is hidden. */
enum { DATA, TEMP, VOLUMES };
static struct volume volumes[VOLUMES];
static unsigned char temporary[TEMP_BYTES] __attribute__((aligned(16)));
/* CPU-only buffers are cached: every file read and write copies through them. */
static unsigned char *transfer;

enum { SESSION_BYTES = 32 * 1024 * 1024 };

static unsigned char *session_memory;
static bool session_only;

enum storage { STORAGE_RAM, STORAGE_DISK, STORAGE_INSTALLED };

/* sos2.storage=ram, disk (the default) or installed: the installed system's
 * data volume, which follows its app store in the cache partition
 * (include/system_store.h) and only a boot that read its system from there
 * can reach. That system shares the RAM image's resources, policy included. */
static enum storage storage_requested(const struct platform_devices *devices) {
    /* Also carry the policy in the immutable boot bundle: loader-added Android
     * arguments can exceed the bounded command-line handoff. Losing that text
     * must never turn a RAM-only image into one that accesses persistent media. */
    const struct runtime_file *policy = runtime_file_find("/system/storage-mode");
    if (policy && (policy->size != 4 || memcmp(runtime_file_data(policy), "RAM1", 4)))
        panic("invalid storage policy");
    const char *args = devices ? devices->bootargs : "";
    bool seen = false;
    enum storage mode = STORAGE_DISK;
    while (*args) {
        args += strspn(args, " \t");
        size_t n = strcspn(args, " \t");
        const char *token = args;
        args += n;
        static const char prefix[] = "sos2.storage=";
        const size_t skip = sizeof(prefix) - 1;
        if (n < skip || memcmp(token, prefix, skip)) continue;
        if (seen) panic("duplicate sos2.storage mode");
        seen = true;
        if (n == skip + 3 && !memcmp(token + skip, "ram", 3)) mode = STORAGE_RAM;
        else if (n == skip + 9 && !memcmp(token + skip, "installed", 9)) mode = STORAGE_INSTALLED;
        else if (n != skip + 4 || memcmp(token + skip, "disk", 4))
            panic("unknown sos2.storage mode");
    }
    if (policy && seen && mode == STORAGE_DISK) panic("conflicting storage policy");
    return policy && !seen ? STORAGE_RAM : mode;
}

static bool range(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, lfs_size_t size) {
    return block < c->block_count && off <= BLOCK && size <= BLOCK - off && !(off % 512) &&
           !(size % 512);
}

/* RAM volumes never block, but littlefs extends a file (truncate or a write
 * past EOF) one byte per call. Yield periodically like the block devices do,
 * so a large fill cannot hold CPU 0. A no-op outside the service worker. */
static uint64_t ram_yield_at;

static void ram_yield(void) {
    if (time_us() - ram_yield_at < 2000) return;
    kernel_io_wait();
    ram_yield_at = time_us();
}

static int read_block(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *data,
                      lfs_size_t size) {
    if (!range(c, block, off, size)) return LFS_ERR_IO;
    struct volume *v = c->context;
    if (v->memory) {
        ram_yield();
        memcpy(data, v->memory + block * BLOCK + off, size);
        return 0;
    }
    return block_read((uint64_t)block * 8 + off / 512, data, size / 512) ? 0 : LFS_ERR_IO;
}

static int write_block(const struct lfs_config *c, lfs_block_t block, lfs_off_t off,
                       const void *data, lfs_size_t size) {
    if (!range(c, block, off, size)) return LFS_ERR_IO;
    struct volume *v = c->context;
    if (v->memory) {
        ram_yield();
        memcpy(v->memory + block * BLOCK + off, data, size);
        return 0;
    }
    return block_write((uint64_t)block * 8 + off / 512, data, size / 512) ? 0 : LFS_ERR_IO;
}

static int erase_block(const struct lfs_config *c, lfs_block_t block) {
    if (block >= c->block_count) return LFS_ERR_IO;
    struct volume *v = c->context;
    if (v->memory) {
        ram_yield();
        memset(v->memory + block * BLOCK, 0xff, BLOCK);
    }
    /* Sector devices overwrite directly. littlefs permits undefined erased
     * contents and validates erased metadata with its forward CRC. */
    return 0;
}

static int sync_blocks(const struct lfs_config *c) {
    struct volume *v = c->context;
    return v->memory || block_sync() ? 0 : LFS_ERR_IO;
}

static void configure(struct volume *v, unsigned count) {
    v->config = (struct lfs_config){.context = v,
                                    .read = read_block,
                                    .prog = write_block,
                                    .erase = erase_block,
                                    .sync = sync_blocks,
                                    .read_size = 512,
                                    .prog_size = 512,
                                    .block_size = BLOCK,
                                    .block_count = count,
                                    .block_cycles = -1,
                                    .cache_size = CACHE,
                                    .lookahead_size = sizeof(v->lookahead),
                                    .read_buffer = v->read_cache,
                                    .prog_buffer = v->write_cache,
                                    .lookahead_buffer = v->lookahead};
}

/* Settings must save even when pages have filled the data volume. A reserve
 * file keeps space aside; a settings write that runs out of space releases it
 * and retries, and the reserve is restored when space is available again. */
static const char reserve_path[] = "/settings/reserve";

static void ensure_reserve(struct volume *v) {
    lfs_file_t file;
    unsigned char cache[CACHE];
    struct lfs_file_config config = {.buffer = cache};
    if (lfs_file_opencfg(&v->fs, &file, reserve_path, LFS_O_RDWR | LFS_O_CREAT, &config)) return;
    if (lfs_file_size(&v->fs, &file) < RESERVE_BYTES)
        lfs_file_truncate(&v->fs, &file, RESERVE_BYTES);
    lfs_file_close(&v->fs, &file);
}

/* The installed system owns its volume: create what an SOS2 volume holds. */
static bool installed_layout(struct volume *v) {
    int settings = lfs_mkdir(&v->fs, "/settings"), profiles = lfs_mkdir(&v->fs, "/profiles");
    return (!settings || settings == LFS_ERR_EXIST) && (!profiles || profiles == LFS_ERR_EXIST);
}

void fs_init(const struct platform_devices *devices) {
    enum storage mode = storage_requested(devices);
    if (!transfer && !(transfer = vm_cpu_alloc((USER_FILE_READ_MAX + PAGE_SIZE - 1) / PAGE_SIZE)))
        panic("file transfer buffer allocation");
    /* Never reuse a prior session's files or credentials on reinitialization.
     * The allocator zeroes new allocations. */
    if (session_memory) {
        vm_cpu_free(session_memory, SESSION_BYTES / PAGE_SIZE);
        session_memory = 0;
    }
    memset(volumes, 0, sizeof(volumes)); /* Per-process file tables are allocated zeroed. */
    /* Without its system store, which an eMMC failure stops, the installed
     * system has no volume. */
    if (mode == STORAGE_INSTALLED && !system_store_sectors(SYSTEM_CACHE)) mode = STORAGE_RAM;
    /* Decide before even probing the controller. A RAM session must also
     * leave an attached, valid SOS2 disk entirely untouched. */
    bool disk = mode != STORAGE_RAM && block_init(devices);
    uint64_t sectors = disk ? block_sectors() : 0;
    struct volume *v = &volumes[DATA];
    if (disk && sectors >= 4096 && sectors <= 2u * 1024 * 1024 && !(sectors % 8)) {
        configure(v, (unsigned)(sectors / 8));
        v->mounted = lfs_mount(&v->fs, &v->config) == 0;
        if (mode == STORAGE_INSTALLED) {
            /* Formatted when it does not mount, as at the first boot. Any
             * failed read has stopped the card, so no format follows one. */
            if (!v->mounted)
                v->mounted = !lfs_format(&v->fs, &v->config) && !lfs_mount(&v->fs, &v->config);
            if (v->mounted && !installed_layout(v)) {
                lfs_unmount(&v->fs);
                v->mounted = false;
            }
        }
        /* Only a volume with /settings and /profiles is an SOS2 volume. */
        struct lfs_info settings, profiles;
        if (v->mounted &&
            (lfs_stat(&v->fs, "/settings", &settings) || settings.type != LFS_TYPE_DIR ||
             lfs_stat(&v->fs, "/profiles", &profiles) || profiles.type != LFS_TYPE_DIR)) {
            lfs_unmount(&v->fs);
            v->mounted = false;
        }
    }
    /* An installed system whose volume is unavailable runs as a RAM session. */
    session_only = mode == STORAGE_RAM || (mode == STORAGE_INSTALLED && !v->mounted);
    if (session_only) {
        session_memory = vm_cpu_alloc(SESSION_BYTES / PAGE_SIZE);
        if (!session_memory) panic("not enough memory for session storage");
        memset(v, 0, sizeof(*v));
        v->memory = session_memory;
        configure(v, SESSION_BYTES / BLOCK);
        if (lfs_format(&v->fs, &v->config) || lfs_mount(&v->fs, &v->config) ||
            lfs_mkdir(&v->fs, "/settings") || lfs_mkdir(&v->fs, "/profiles"))
            panic("session filesystem initialization");
        v->mounted = true;
    }
    if (v->mounted) ensure_reserve(v);
    v = &volumes[TEMP];
    v->memory = temporary;
    configure(v, TEMP_BYTES / BLOCK);
    if (lfs_format(&v->fs, &v->config) || lfs_mount(&v->fs, &v->config) ||
        lfs_mkdir(&v->fs, "/profiles"))
        panic("temporary filesystem initialization");
    v->mounted = true;
}

static const char profiles[] = "/profiles/";

static void profile_root(const struct fs_space *s, char out[INTERNAL_PATH]) {
    memcpy(out, profiles, sizeof(profiles) - 1);
    strcpy(out + sizeof(profiles) - 1, s->profile);
}

bool fs_space_init(struct fs_space *s, const char *profile) {
    *s = (struct fs_space){0};
    size_t length = strlen(profile);
    if (!length || length >= FS_PROFILE_MAX) return false;
    /* fs_space_close releases the tables, including after a failed start. */
    struct fs_pool *pool = vm_cpu_alloc(POOL_PAGES);
    if (!pool) return false;
    s->pool = pool;
    strcpy(s->profile, profile);
    pool->next = pools;
    pools = pool;
    char root[INTERNAL_PATH];
    profile_root(s, root);
    /* Each component below /profiles: apps, then apps/<id>. If a full or
     * failed volume cannot make them, only file operations there fail. */
    for (unsigned index = 0; index < VOLUMES; ++index) {
        struct volume *v = &volumes[index];
        if (!v->mounted) continue;
        for (char *end = root + sizeof(profiles) - 1;; ++end) {
            if (*end && *end != '/') continue;
            char saved = *end;
            *end = 0;
            int result = lfs_mkdir(&v->fs, root);
            *end = saved;
            if (!saved || (result && result != LFS_ERR_EXIST)) break;
        }
    }
    return true;
}

/* A directory and everything below it, deepest first: littlefs removes only
 * empty directories, so the directory is read again after each removal. */
static bool remove_tree(struct volume *v, char path[INTERNAL_PATH]) {
    size_t root = strlen(path);
    for (;;) {
        lfs_dir_t dir;
        struct lfs_info info;
        size_t length = strlen(path);
        int err = lfs_dir_open(&v->fs, &dir, path);
        if (err) return err == LFS_ERR_NOENT && length == root;
        bool found = false;
        while (!found && (err = lfs_dir_read(&v->fs, &dir, &info)) > 0)
            found = strcmp(info.name, ".") && strcmp(info.name, "..");
        if (lfs_dir_close(&v->fs, &dir) || err < 0) return false;
        if (!found) {
            if (lfs_remove(&v->fs, path)) return false;
            if (length == root) return true;
            while (path[length] != '/') --length;
            path[length] = 0;
            continue;
        }
        size_t name = strlen(info.name);
        if (length + 1 + name >= INTERNAL_PATH) return false;
        path[length] = '/';
        memcpy(path + length + 1, info.name, name + 1);
        if (info.type == LFS_TYPE_DIR) continue;
        err = lfs_remove(&v->fs, path);
        path[length] = 0;
        if (err) return false;
    }
}

bool fs_profile_remove(const char *profile) {
    size_t length = strlen(profile);
    if (!length || length >= FS_PROFILE_MAX) return false;
    bool removed = true;
    for (unsigned index = 0; index < VOLUMES; ++index) {
        char path[INTERNAL_PATH];
        memcpy(path, profiles, sizeof(profiles) - 1);
        strcpy(path + sizeof(profiles) - 1, profile);
        if (volumes[index].mounted && !remove_tree(&volumes[index], path)) removed = false;
    }
    return removed;
}

bool fs_persistent_ready(void) { return !session_only && volumes[DATA].mounted; }

bool fs_session_only(void) { return session_only; }

static const char *const setting_paths[FS_SETTING_COUNT] = {"/settings/wifi", "/settings/clock"};
static const char *const pending_paths[FS_SETTING_COUNT] = {"/settings/wifi.pending",
                                                            "/settings/clock.pending"};

int32_t fs_settings_read(enum fs_setting key, void *out, unsigned capacity) {
    if ((unsigned)key >= FS_SETTING_COUNT) return USER_EINVAL;
    struct volume *v = &volumes[DATA];
    if (!v->mounted) return USER_ENODEV;
    if (!out || !capacity || capacity > 1024) return USER_EINVAL;
    lfs_file_t file;
    unsigned char cache[CACHE];
    struct lfs_file_config config = {.buffer = cache};
    int result = lfs_file_opencfg(&v->fs, &file, setting_paths[key], LFS_O_RDONLY, &config);
    if (result) return result == LFS_ERR_CORRUPT ? USER_EIO : result;
    lfs_soff_t size = lfs_file_size(&v->fs, &file);
    if (size < 0 || size > (lfs_soff_t)capacity) result = USER_EIO;
    else {
        result = lfs_file_read(&v->fs, &file, out, (unsigned)size);
        if (result != size) result = USER_EIO;
    }
    int closed = lfs_file_close(&v->fs, &file);
    if (result >= 0 && closed) result = USER_EIO;
    volatile unsigned char *wipe = cache;
    for (unsigned i = 0; i < sizeof(cache); ++i) wipe[i] = 0;
    return result;
}

static int write_setting(struct volume *v, enum fs_setting key, const void *data, unsigned size) {
    lfs_file_t file;
    unsigned char cache[CACHE];
    struct lfs_file_config config = {.buffer = cache};
    int result = lfs_file_opencfg(&v->fs, &file, pending_paths[key],
                                  LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &config);
    if (result) return result;
    lfs_ssize_t written = lfs_file_write(&v->fs, &file, data, size);
    result = written == (lfs_ssize_t)size ? 0 : written < 0 ? (int)written : LFS_ERR_IO;
    if (!result) result = lfs_file_sync(&v->fs, &file);
    int closed = lfs_file_close(&v->fs, &file);
    if (!result) result = closed;
    volatile unsigned char *wipe = cache;
    for (unsigned i = 0; i < sizeof(cache); ++i) wipe[i] = 0;
    /* littlefs atomically replaces the committed name. A failed write never
     * removes the old settings; both old and new files stay kernel-private. */
    if (!result) result = lfs_rename(&v->fs, pending_paths[key], setting_paths[key]);
    return result;
}

int32_t fs_settings_write(enum fs_setting key, const void *data, unsigned size) {
    if ((unsigned)key >= FS_SETTING_COUNT) return USER_EINVAL;
    struct volume *v = &volumes[DATA];
    if (!v->mounted) return USER_ENODEV;
    if (!data || !size || size > 1024) return USER_EINVAL;
    int result = write_setting(v, key, data, size);
    if (result == LFS_ERR_NOSPC) {
        lfs_remove(&v->fs, pending_paths[key]);
        if (!lfs_remove(&v->fs, reserve_path)) result = write_setting(v, key, data, size);
    }
    if (result) lfs_remove(&v->fs, pending_paths[key]);
    ensure_reserve(v);
    if (!result && sync_blocks(&v->config)) result = LFS_ERR_IO;
    return result ? USER_EIO : 0;
}

static int error(int value) { return value == LFS_ERR_CORRUPT ? USER_EIO : value; }

static int close_handle(struct fs_handle *h) {
    int result = 0;
    if (h->kind == DISK_FILE) {
        struct inode *n = h->object;
        /* A lost inode's littlefs file is already closed. */
        result = n->lost ? USER_EIO : lfs_file_sync(&n->volume->fs, &n->file);
        if (!--n->refs) {
            int closed = n->lost ? 0 : lfs_file_close(&n->volume->fs, &n->file);
            if (!result) result = closed;
            n->volume = 0;
        }
    } else if (h->kind == DISK_DIR) {
        struct directory *dir = h->object;
        result = lfs_dir_close(&dir->volume->fs, &dir->dir);
        dir->used = false;
    }
    *h = (struct fs_handle){0};
    return error(result);
}

/* SYS_FILE_LOCK, for SQLite across the pages sharing the browser profile: a
 * /data or /tmp path has at most one holding process, and a lock ends with
 * its process. Taking a lock held elsewhere fails with EBUSY. A process holds
 * at most a few, so one cannot take them all. */
enum { FILE_LOCKS = 32, PROCESS_FILE_LOCKS = 4 };

static struct file_lock {
    const struct fs_space *holder;
    struct volume *volume;
    char path[INTERNAL_PATH];
} file_locks[FILE_LOCKS];

void fs_space_close(struct fs_space *s) {
    for (unsigned i = 0; i < FILE_LOCKS; ++i)
        if (file_locks[i].holder == s) file_locks[i].holder = 0;
    for (unsigned i = 0; i < USER_FILE_HANDLES; ++i)
        if (s->handles[i].kind) (void)close_handle(&s->handles[i]);
    if (s->pool) {
        struct fs_pool **at = &pools;
        while (*at && *at != s->pool) at = &(*at)->next;
        if (*at) *at = s->pool->next;
        vm_cpu_free(s->pool, POOL_PAGES);
        s->pool = 0;
    }
}

static int path_from_user(struct vm_space *vm, char *path, uint32_t address, uint32_t length) {
    if (!length || length > USER_PATH_MAX) return USER_EINVAL;
    if (!vm_copy_from_user(vm, path, address, length)) return USER_EFAULT;
    if (path[0] != '/' || memchr(path, 0, length)) return USER_EINVAL;
    path[length] = 0;
    /* Canonical absolute paths only: normalization belongs in the C runtime. */
    for (unsigned start = 1; start < length;) {
        unsigned end = start;
        while (end < length && path[end] != '/') ++end;
        unsigned n = end - start;
        if (!n || (n == 1 && path[start] == '.') ||
            (n == 2 && path[start] == '.' && path[start + 1] == '.'))
            return USER_EINVAL;
        if (end == length - 1) return USER_EINVAL;
        start = end + 1;
    }
    return 0;
}

static struct volume *volume_for(struct fs_space *s, const char *path,
                                 char relative[INTERNAL_PATH]) {
    if (!s->profile[0]) return 0;
    unsigned index, skip;
    if (!memcmp(path, "/data", 5) && (!path[5] || path[5] == '/')) {
        index = DATA;
        skip = 5;
    } else if (!memcmp(path, "/tmp", 4) && (!path[4] || path[4] == '/')) {
        index = TEMP;
        skip = 4;
    } else return 0;
    profile_root(s, relative);
    strcpy(relative + strlen(relative), path + skip);
    return &volumes[index];
}

static struct inode *open_inode(struct fs_space *s, struct volume *v, const char *relative) {
    for (unsigned i = 0; i < USER_FILE_HANDLES; ++i) {
        struct inode *n = &s->pool->inodes[i];
        if (n->volume == v && !n->lost && !strcmp(n->path, relative)) return n;
    }
    return 0;
}

/* Each process has its own littlefs file for a path, which keeps the contents
 * it opened. So the lock hands contents over: releasing commits the holder's
 * writes, and taking reopens the taker's file if another process committed
 * to it since (littlefs marks it "dusty"), for all its descriptors. */
static int32_t file_lock(struct fs_space *s, const char *path, uint32_t take) {
    if (take > 1) return USER_EINVAL;
    char relative[INTERNAL_PATH];
    struct volume *v = volume_for(s, path, relative);
    if (!v || !v->mounted) return USER_EINVAL;
    struct file_lock *unused = 0;
    unsigned held = 0;
    for (unsigned i = 0; i < FILE_LOCKS; ++i) {
        struct file_lock *l = &file_locks[i];
        if (!l->holder) {
            if (!unused) unused = l;
            continue;
        }
        held += l->holder == s;
        if (l->volume != v || strcmp(l->path, relative)) continue;
        if (l->holder != s) return take ? USER_EBUSY : USER_EINVAL;
        if (take) return 0;
        struct inode *n = open_inode(s, v, relative);
        int result = n && (n->file.flags & (LFS_F_DIRTY | LFS_F_WRITING))
                         ? error(lfs_file_sync(&v->fs, &n->file))
                         : 0;
        l->holder = 0;
        return result;
    }
    if (!take) return USER_EINVAL;
    if (!unused || held >= PROCESS_FILE_LOCKS) return USER_EAGAIN;
    struct inode *n = open_inode(s, v, relative);
    if (n && (n->file.flags & LFS_F_DUSTY)) {
        lfs_file_close(&v->fs, &n->file);
        if (lfs_file_opencfg(&v->fs, &n->file, relative, LFS_O_RDWR, &n->config)) {
            n->lost = true;
            n->path[0] = 0;
            return USER_EIO;
        }
    }
    unused->holder = s;
    unused->volume = v;
    strcpy(unused->path, relative);
    return 0;
}

/* Firmware, network provisioning and board calibration are kernel-only.
 * These bundle pages are supervisor mappings; hide all corresponding paths
 * from file opens, stat and directory enumeration as well. */
static bool private_resource(const char *path) {
    size_t n = strlen(path);
    return !strcmp(path, "/system") || (n >= 8 && !memcmp(path, "/system/", 8)) ||
           !strcmp(path, "/firmware") || (n >= 10 && !memcmp(path, "/firmware/", 10));
}

static const struct runtime_file *public_file(const char *path) {
    return private_resource(path) ? 0 : runtime_file_find(path);
}

static bool resource_dir(const char *path) {
    if (private_resource(path)) return false;
    if (!strcmp(path, "/")) return true;
    size_t n = strlen(path);
    for (unsigned i = 0; i < runtime_files_count(); ++i) {
        const char *name = runtime_file_at(i)->name;
        if (private_resource(name)) continue;
        if (strlen(name) > n && !memcmp(name, path, n) && name[n] == '/') return true;
    }
    return false;
}

static int path_info(struct fs_space *s, const char *path, struct user_file_info *info) {
    char relative[INTERNAL_PATH];
    struct volume *v = volume_for(s, path, relative);
    if (v) {
        if (!v->mounted) return USER_ENODEV;
        /* littlefs commits directory metadata at sync; stat must also see a
           dirty file still held by this process through another descriptor. */
        for (unsigned i = 0; i < USER_FILE_HANDLES; ++i) {
            struct inode *node = &s->pool->inodes[i];
            if (node->refs && node->volume == v && !strcmp(node->path, relative)) {
                lfs_soff_t size = lfs_file_size(&v->fs, &node->file);
                if (size < 0) return error(size);
                *info = (struct user_file_info){FILE_REGULAR, (uint32_t)size};
                return 0;
            }
        }
        struct lfs_info result;
        int err = lfs_stat(&v->fs, relative, &result);
        if (err) return error(err);
        *info = (struct user_file_info){result.type == LFS_TYPE_DIR ? FILE_DIR : FILE_REGULAR,
                                        result.size};
        return 0;
    }
    const struct runtime_file *file = public_file(path);
    if (file) {
        *info = (struct user_file_info){FILE_REGULAR, file->size};
        return 0;
    }
    if (resource_dir(path)) {
        *info = (struct user_file_info){FILE_DIR, 0};
        return 0;
    }
    return USER_ENOENT;
}

static int open_file(struct fs_space *s, const char *path, unsigned flags) {
    if (!flags) flags = FILE_READ;
    if ((flags & ~127u) || !(flags & (FILE_READ | FILE_WRITE)) ||
        ((flags & (FILE_CREATE | FILE_TRUNCATE | FILE_APPEND)) && !(flags & FILE_WRITE)) ||
        ((flags & FILE_EXCLUSIVE) && !(flags & FILE_CREATE)))
        return USER_EINVAL;
    unsigned index = 0;
    while (index < USER_FILE_HANDLES && s->handles[index].kind) ++index;
    if (index == USER_FILE_HANDLES) return USER_EMFILE;
    struct fs_handle *h = &s->handles[index];
    char relative[INTERNAL_PATH];
    struct volume *v = volume_for(s, path, relative);
    if (!v) {
        if (flags & ~(FILE_READ | FILE_DIRECTORY)) return USER_EROFS;
        const struct runtime_file *file = public_file(path);
        if (flags & FILE_DIRECTORY) {
            if (!resource_dir(path)) return file ? USER_ENOTDIR : USER_ENOENT;
            h->kind = RESOURCE_DIR;
            strcpy(h->path, path);
        } else {
            if (!file) return resource_dir(path) ? USER_EISDIR : USER_ENOENT;
            h->kind = RESOURCE;
            h->object = (void *)file;
        }
    } else {
        if (!v->mounted) return USER_ENODEV;
        if (flags & FILE_DIRECTORY) {
            if (flags != (FILE_READ | FILE_DIRECTORY)) return USER_EINVAL;
            unsigned i = 0;
            while (i < USER_FILE_HANDLES && s->pool->directories[i].used) ++i;
            if (i == USER_FILE_HANDLES) return USER_EMFILE;
            struct directory *dir = &s->pool->directories[i];
            int err = lfs_dir_open(&v->fs, &dir->dir, relative);
            if (err) return error(err);
            dir->used = true;
            dir->volume = v;
            h->kind = DISK_DIR;
            h->object = dir;
            strcpy(h->path, path);
        } else {
            struct inode *n = 0, *inodes = s->pool->inodes;
            for (unsigned i = 0; i < USER_FILE_HANDLES; ++i)
                if (inodes[i].volume == v && !strcmp(inodes[i].path, relative)) n = &inodes[i];
            if (n) {
                if ((flags & (FILE_CREATE | FILE_EXCLUSIVE)) == (FILE_CREATE | FILE_EXCLUSIVE))
                    return USER_EEXIST;
                if (flags & FILE_TRUNCATE) {
                    int err = lfs_file_truncate(&v->fs, &n->file, 0);
                    if (err) return error(err);
                }
            } else {
                for (unsigned i = 0; i < USER_FILE_HANDLES; ++i)
                    if (!inodes[i].volume) {
                        n = &inodes[i];
                        break;
                    }
                if (!n) return USER_EMFILE;
                *n = (struct inode){.volume = v};
                n->config.buffer = n->cache;
                int options = LFS_O_RDWR | ((flags & FILE_CREATE) ? LFS_O_CREAT : 0) |
                              ((flags & FILE_EXCLUSIVE) ? LFS_O_EXCL : 0) |
                              ((flags & FILE_TRUNCATE) ? LFS_O_TRUNC : 0);
                int err = lfs_file_opencfg(&v->fs, &n->file, relative, options, &n->config);
                if (err) {
                    n->volume = 0;
                    return error(err);
                }
                strcpy(n->path, relative);
            }
            ++n->refs;
            h->kind = DISK_FILE;
            h->object = n;
        }
    }
    h->flags = flags;
    h->offset = 0;
    return (int)index + 1;
}

static int file_info(struct fs_handle *h, struct user_file_info *info) {
    if (h->kind == RESOURCE)
        *info = (struct user_file_info){FILE_REGULAR, ((struct runtime_file *)h->object)->size};
    else if (h->kind == DISK_FILE) {
        struct inode *n = h->object;
        if (n->lost) return USER_EIO;
        lfs_soff_t size = lfs_file_size(&n->volume->fs, &n->file);
        if (size < 0) return error(size);
        *info = (struct user_file_info){FILE_REGULAR, (uint32_t)size};
    } else *info = (struct user_file_info){FILE_DIR, 0};
    return 0;
}

static bool child_name(const char *parent, unsigned index, struct user_dir_entry *entry) {
    const struct runtime_file *f = runtime_file_at(index);
    if (!f || private_resource(f->name)) return false;
    size_t n = strlen(parent);
    if (memcmp(parent, f->name, n) || (n > 1 && f->name[n] != '/')) return false;
    const char *child = f->name + n + (n > 1);
    if (!*child) return false;
    unsigned len = 0;
    while (child[len] && child[len] != '/') ++len;
    *entry = (struct user_dir_entry){
        child[len] ? FILE_DIR : FILE_REGULAR, child[len] ? 0 : f->size, {0}};
    memcpy(entry->name, child, len);
    return true;
}

static int read_directory(struct fs_handle *h, struct user_dir_entry *entry) {
    if (h->kind == DISK_DIR) {
        struct directory *dir = h->object;
        struct lfs_info info;
        int err;
        err = lfs_dir_read(&dir->volume->fs, &dir->dir, &info);
        if (err <= 0) return error(err);
        *entry = (struct user_dir_entry){
            info.type == LFS_TYPE_DIR ? FILE_DIR : FILE_REGULAR, info.size, {0}};
        strcpy(entry->name, info.name);
        return 1;
    }
    if (h->kind != RESOURCE_DIR) return USER_ENOTDIR;
    bool root = !strcmp(h->path, "/");
    if (root && h->offset < 2) {
        *entry = (struct user_dir_entry){FILE_DIR, 0, {0}};
        strcpy(entry->name, h->offset++ ? "tmp" : "data");
        return 1;
    }
    while (h->offset < (root ? 2u : 0u) + runtime_files_count()) {
        unsigned index = h->offset++ - (root ? 2 : 0);
        if (!child_name(h->path, index, entry)) continue;
        if (root && (!strcmp(entry->name, "data") || !strcmp(entry->name, "tmp"))) continue;
        bool duplicate = false;
        for (unsigned i = 0; i < index; ++i) {
            struct user_dir_entry before;
            if (child_name(h->path, i, &before) && !strcmp(before.name, entry->name)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) return 1;
    }
    return 0;
}

/* A removed or replaced file: littlefs gives its blocks back to the
 * allocator unless a handle has unsynced writes, so no handle, in any
 * process, may read them again. Each becomes lost (EIO), its name forgotten,
 * so the next open finds the file now at that path. */
static void forget_file(struct volume *v, const char *relative) {
    for (struct fs_pool *p = pools; p; p = p->next)
        for (unsigned i = 0; i < USER_FILE_HANDLES; ++i) {
            struct inode *n = &p->inodes[i];
            if (n->volume != v || n->lost || strcmp(n->path, relative)) continue;
            lfs_file_close(&v->fs, &n->file);
            n->lost = true;
            n->path[0] = 0;
        }
}

static int path_operation(struct fs_space *s, uint32_t number, const char *path,
                          const char *other) {
    char relative[INTERNAL_PATH], new_relative[INTERNAL_PATH];
    struct volume *v = volume_for(s, path, relative);
    if (!v) return USER_EROFS;
    if (!v->mounted) return USER_ENODEV;
    if (!strcmp(path, "/data") || !strcmp(path, "/tmp")) return USER_EBUSY;
    if (number == SYS_PATH_MKDIR) return error(lfs_mkdir(&v->fs, relative));
    if (number == SYS_PATH_REMOVE) {
        int err = lfs_remove(&v->fs, relative);
        if (!err) forget_file(v, relative);
        return error(err);
    }
    struct volume *new_volume = volume_for(s, other, new_relative);
    if (new_volume != v) return USER_EXDEV;
    if (!strcmp(other, "/data") || !strcmp(other, "/tmp")) return USER_EBUSY;
    /* Update every open inode name only after a successful atomic rename. */
    size_t old_length = strlen(relative), new_length = strlen(new_relative);
    /* littlefs would detach a directory moved below itself, losing the whole
     * subtree and leaking its blocks permanently. Paths are canonical here. */
    if (new_length > old_length && !memcmp(new_relative, relative, old_length) &&
        new_relative[old_length] == '/')
        return USER_EINVAL;
    /* Every process's open files, since pages share the browser profile. */
    for (struct fs_pool *p = pools; p; p = p->next)
        for (unsigned i = 0; i < USER_FILE_HANDLES; ++i) {
            struct inode *n = &p->inodes[i];
            if (n->volume == v && !memcmp(n->path, relative, old_length) &&
                (!n->path[old_length] || n->path[old_length] == '/') &&
                new_length + strlen(n->path + old_length) >= INTERNAL_PATH)
                return USER_ENAMETOOLONG;
        }
    /* Publish dirty inline data before moving its directory entry. Otherwise
     * a later file close can commit the pre-rename entry and lose the write. */
    for (struct fs_pool *p = pools; p; p = p->next)
        for (unsigned i = 0; i < USER_FILE_HANDLES; ++i) {
            struct inode *n = &p->inodes[i];
            if (n->refs && n->volume == v && !n->lost &&
                ((!memcmp(n->path, relative, old_length) &&
                  (!n->path[old_length] || n->path[old_length] == '/')) ||
                 !strcmp(n->path, new_relative))) {
                int result = lfs_file_sync(&v->fs, &n->file);
                if (result) return error(result);
            }
        }
    int err = lfs_rename(&v->fs, relative, new_relative);
    if (!err && strcmp(relative, new_relative))
        forget_file(v, new_relative); /* the replaced file */
    if (!err && strcmp(relative, new_relative))
        for (struct fs_pool *p = pools; p; p = p->next)
            for (unsigned i = 0; i < USER_FILE_HANDLES; ++i)
                if (p->inodes[i].volume == v && !p->inodes[i].lost) {
                    struct inode *n = &p->inodes[i];
                    char *name = n->path;
                    if (!memcmp(name, relative, old_length) &&
                        (!name[old_length] || name[old_length] == '/')) {
                        bool moved = !name[old_length];
                        char updated[INTERNAL_PATH];
                        strcpy(updated, new_relative);
                        strcpy(updated + new_length, name + old_length);
                        strcpy(name, updated);
                        /* littlefs detaches an open file whose own entry moves, so later
                         * writes and syncs would silently persist nothing. Its data was
                         * synced above: reopen it under the new name. Files inside a
                         * renamed directory keep valid handles. */
                        if (moved) {
                            lfs_file_close(&v->fs, &n->file);
                            if (lfs_file_opencfg(&v->fs, &n->file, name, LFS_O_RDWR, &n->config)) {
                                n->lost = true;
                                name[0] = 0;
                            }
                        }
                    }
                }
    return error(err);
}

/* Seeking only records an offset and makes no littlefs call, so it runs on
 * the syscall path instead of pausing the page for the service worker. A
 * process is frozen while its own service I/O runs, so its handles are idle. */
int32_t fs_seek(struct fs_space *s, uint32_t handle, uint32_t offset) {
    if (!handle || handle > USER_FILE_HANDLES || !s->handles[handle - 1].kind) return USER_EBADF;
    struct fs_handle *h = &s->handles[handle - 1];
    uint32_t limit;
    if (h->kind == RESOURCE) limit = ((struct runtime_file *)h->object)->size;
    else if (h->kind == DISK_FILE) {
        if (((struct inode *)h->object)->lost) return USER_EIO;
        limit = LFS_FILE_MAX;
    } else return USER_EISDIR;
    if (offset > limit) return USER_EINVAL;
    h->offset = offset;
    return (int32_t)offset;
}

int32_t fs_call(struct fs_space *s, struct vm_space *vm, uint32_t number, uint32_t a, uint32_t b,
                uint32_t c, uint32_t d) {
    char path[128], other[128];
    if (number == SYS_FILE_OPEN || number == SYS_PATH_MKDIR || number == SYS_PATH_REMOVE ||
        number == SYS_PATH_RENAME || number == SYS_PATH_INFO || number == SYS_FS_INFO ||
        number == SYS_FILE_LOCK) {
        int err = path_from_user(vm, path, a, b);
        if (err) return err;
        if (number == SYS_FILE_OPEN) return open_file(s, path, c);
        if (number == SYS_FILE_LOCK) return file_lock(s, path, c);
        if (number == SYS_PATH_INFO) {
            struct user_file_info info;
            err = path_info(s, path, &info);
            if (err) return err;
            return vm_copy_to_user(vm, c, &info, sizeof(info)) ? 0 : USER_EFAULT;
        }
        if (number == SYS_FS_INFO) {
            struct user_file_info file;
            err = path_info(s, path, &file);
            if (err) return err;
            char relative[INTERNAL_PATH];
            struct volume *volume = volume_for(s, path, relative);
            struct user_fs_info info = {BLOCK, (runtime_files_size() + BLOCK - 1) / BLOCK, 0,
                                        USER_PATH_MAX, 1};
            if (volume) {
                lfs_ssize_t used = lfs_fs_size(&volume->fs);
                if (used < 0) return error(used);
                if ((uint32_t)used > volume->config.block_count) return USER_EIO;
                info.blocks = volume->config.block_count;
                info.free_blocks = info.blocks - used;
                info.read_only = 0;
            }
            return vm_copy_to_user(vm, c, &info, sizeof(info)) ? 0 : USER_EFAULT;
        }
        if (number == SYS_PATH_RENAME) {
            err = path_from_user(vm, other, c, d);
            if (err) return err;
        }
        return path_operation(s, number, path, other);
    }
    if (!a || a > USER_FILE_HANDLES || !s->handles[a - 1].kind) return USER_EBADF;
    struct fs_handle *h = &s->handles[a - 1];
    /* Closing never depends on the file's state, so a failed handle is always released. */
    if (number == SYS_FILE_CLOSE) return close_handle(h);
    struct user_file_info info;
    int err = file_info(h, &info);
    if (err) return err;
    struct inode *n = h->kind == DISK_FILE ? h->object : 0;
    switch (number) {
        case SYS_FILE_SIZE: return info.type == FILE_REGULAR ? (int32_t)info.size : USER_EISDIR;
        case SYS_FILE_INFO: return vm_copy_to_user(vm, b, &info, sizeof(info)) ? 0 : USER_EFAULT;
        case SYS_FILE_NEXT: {
            if (!vm_accessible(vm, b, sizeof(struct user_dir_entry), VM_WRITE)) return USER_EFAULT;
            struct user_dir_entry entry;
            int result = read_directory(h, &entry);
            if (result > 0 && !vm_copy_to_user(vm, b, &entry, sizeof(entry))) return USER_EFAULT;
            return result;
        }
        case SYS_FILE_READ: {
            if (info.type != FILE_REGULAR) return USER_EISDIR;
            if (!(h->flags & FILE_READ)) return USER_EBADF;
            if (c > USER_FILE_READ_MAX) return USER_EINVAL;
            uint32_t count = h->offset >= info.size ? 0 : info.size - h->offset;
            if (count > c) count = c;
            if (!count) return 0;
            if (!vm_accessible(vm, b, count, VM_WRITE)) return USER_EFAULT;
            const void *data;
            if (n) {
                err = lfs_file_seek(&n->volume->fs, &n->file, h->offset, LFS_SEEK_SET);
                if (err < 0) return error(err);
                err = lfs_file_read(&n->volume->fs, &n->file, transfer, count);
                if (err < 0) return error(err);
                count = (unsigned)err;
                data = transfer;
            } else data = (const unsigned char *)runtime_file_data(h->object) + h->offset;
            if (!vm_copy_to_user(vm, b, data, count)) return USER_EFAULT;
            h->offset += count;
            return (int32_t)count;
        }
        case SYS_FILE_WRITE:
            if (info.type != FILE_REGULAR) return USER_EISDIR;
            if (!n) return USER_EROFS;
            if (!(h->flags & FILE_WRITE)) return USER_EBADF;
            if (c > USER_FILE_READ_MAX) return USER_EINVAL;
            if (!vm_copy_from_user(vm, transfer, b, c)) return USER_EFAULT;
            if (!c) return 0;
            {
                uint32_t offset = (h->flags & FILE_APPEND) ? info.size : h->offset;
                if (offset > LFS_FILE_MAX || c > LFS_FILE_MAX - offset) return USER_EFBIG;
                err = lfs_file_seek(&n->volume->fs, &n->file, offset, LFS_SEEK_SET);
                if (err < 0) return error(err);
                err = lfs_file_write(&n->volume->fs, &n->file, transfer, c);
                if (err >= 0) h->offset = offset + (unsigned)err;
                return error(err);
            }
        case SYS_FILE_TRUNCATE:
            if (info.type != FILE_REGULAR) return USER_EISDIR;
            if (!n) return USER_EROFS;
            if (!(h->flags & FILE_WRITE)) return USER_EBADF;
            return b > LFS_FILE_MAX ? USER_EFBIG
                                    : error(lfs_file_truncate(&n->volume->fs, &n->file, b));
        case SYS_FILE_SYNC: return n ? error(lfs_file_sync(&n->volume->fs, &n->file)) : 0;
        default: return USER_ENOSYS;
    }
}
