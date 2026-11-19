/* SQLite's native SOS2 VFS. Rollback journals and WAL are durable. Locks
 * coordinate the connections in this process, and a database with a lock in
 * this process also holds a kernel lock (sos2_file_lock): the pages sharing the
 * browser profile take turns, and a lock ends with its process. Taking the
 * kernel lock brings this process's open file up to what another process
 * wrote, and releasing it commits this process's writes. The WAL index lives
 * in process RAM, so a WAL database must not be shared between processes.
 * Dynamic loading is deliberately unavailable.
 */
#include <sqlite3.h>
#include <sos2/services.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct shared_index {
    unsigned references, page_size;
    void *pages[32];
};

struct native_file {
    sqlite3_file base;
    int fd, level, delete_on_close;
    int database; /* A main database in /data or /tmp: it takes the kernel lock. */
    char path[128];
    struct shared_index *shared;
    unsigned shared_locks, exclusive_locks;
    struct native_file *next;
};

static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static struct native_file *opened;
static unsigned temporary_id;

static int error_code(int fallback) {
    return errno == ENOSPC || errno == EFBIG ? SQLITE_FULL : fallback;
}

/* Whether another connection in this process holds a lock on the file. */
static int process_holds(const struct native_file *file) {
    for (struct native_file *other = opened; other; other = other->next)
        if (other != file && other->level > SQLITE_LOCK_NONE && !strcmp(other->path, file->path))
            return 1;
    return 0;
}

/* The kernel lock goes when this process's last lock on a database does. */
static int release(struct native_file *file) {
    if (!file->database || process_holds(file)) return 0;
    return sos2_file_lock(file->path, 0);
}

static int unlock_file(sqlite3_file *base, int level) {
    struct native_file *file = (void *)base;
    int result = SQLITE_OK;
    pthread_mutex_lock(&registry);
    int held = file->level > SQLITE_LOCK_NONE;
    file->level = level;
    if (held && level == SQLITE_LOCK_NONE && release(file)) result = SQLITE_IOERR_UNLOCK;
    pthread_mutex_unlock(&registry);
    return result;
}

static int close_file(sqlite3_file *base) {
    struct native_file *file = (void *)base;
    int result = file->fd >= 0 && close(file->fd) ? SQLITE_IOERR_CLOSE : SQLITE_OK;
    pthread_mutex_lock(&registry);
    if (file->level > SQLITE_LOCK_NONE) {
        file->level = SQLITE_LOCK_NONE;
        (void)release(file);
    }
    pthread_mutex_unlock(&registry);
    if (file->delete_on_close && unlink(file->path) && errno != ENOENT)
        result = SQLITE_IOERR_DELETE;
    pthread_mutex_lock(&registry);
    struct native_file **at = &opened;
    while (*at && *at != file) at = &(*at)->next;
    if (*at) *at = file->next;
    if (file->shared && !--file->shared->references) {
        for (unsigned i = 0; i < 32; ++i) sqlite3_free(file->shared->pages[i]);
        sqlite3_free(file->shared);
    }
    pthread_mutex_unlock(&registry);
    file->base.pMethods = NULL;
    return result;
}

static int seek_file(struct native_file *file, sqlite3_int64 offset) {
    if (offset < 0 || offset > INT_MAX) {
        errno = EFBIG;
        return -1;
    }
    return lseek(file->fd, (off_t)offset, SEEK_SET) < 0 ? -1 : 0;
}

static int read_file(sqlite3_file *base, void *buffer, int count, sqlite3_int64 offset) {
    struct native_file *file = (void *)base;
    if (seek_file(file, offset)) return SQLITE_IOERR_SEEK;
    int total = 0;
    while (total < count) {
        int n = read(file->fd, (char *)buffer + total, count - total);
        if (n < 0) return SQLITE_IOERR_READ;
        if (!n) {
            memset((char *)buffer + total, 0, count - total);
            return SQLITE_IOERR_SHORT_READ;
        }
        total += n;
    }
    return SQLITE_OK;
}

static int write_file(sqlite3_file *base, const void *buffer, int count, sqlite3_int64 offset) {
    struct native_file *file = (void *)base;
    if (seek_file(file, offset)) return error_code(SQLITE_IOERR_SEEK);
    int total = 0;
    while (total < count) {
        int n = write(file->fd, (const char *)buffer + total, count - total);
        if (n <= 0) return error_code(SQLITE_IOERR_WRITE);
        total += n;
    }
    return SQLITE_OK;
}

static int truncate_file(sqlite3_file *base, sqlite3_int64 size) {
    if (size < 0 || size > INT_MAX) return SQLITE_FULL;
    return ftruncate(((struct native_file *)base)->fd, size) ? error_code(SQLITE_IOERR_TRUNCATE)
                                                             : SQLITE_OK;
}

static int sync_file(sqlite3_file *base, int flags) {
    (void)flags;
    return fsync(((struct native_file *)base)->fd) ? SQLITE_IOERR_FSYNC : SQLITE_OK;
}

static int size_file(sqlite3_file *base, sqlite3_int64 *size) {
    struct stat info;
    if (fstat(((struct native_file *)base)->fd, &info)) return SQLITE_IOERR_FSTAT;
    *size = info.st_size;
    return SQLITE_OK;
}

/* A connection's first lock on a database takes the kernel lock, unless
 * another connection here holds it. A full lock table is temporary too. */
static int share(struct native_file *file) {
    if (!file->database || process_holds(file) || !sos2_file_lock(file->path, 1)) return SQLITE_OK;
    return errno == EBUSY || errno == EAGAIN ? SQLITE_BUSY : SQLITE_IOERR_LOCK;
}

static int lock_file(sqlite3_file *base, int level) {
    struct native_file *file = (void *)base;
    int result = SQLITE_OK;
    pthread_mutex_lock(&registry);
    if (level > file->level) {
        for (struct native_file *other = opened; other; other = other->next) {
            if (other == file || strcmp(other->path, file->path)) continue;
            if ((level == SQLITE_LOCK_SHARED && other->level >= SQLITE_LOCK_PENDING) ||
                (level >= SQLITE_LOCK_RESERVED && other->level >= SQLITE_LOCK_RESERVED) ||
                (level == SQLITE_LOCK_EXCLUSIVE && other->level >= SQLITE_LOCK_SHARED)) {
                result = SQLITE_BUSY;
                break;
            }
        }
        if (result == SQLITE_OK && file->level == SQLITE_LOCK_NONE) result = share(file);
        if (result == SQLITE_OK) file->level = level;
        else if (level == SQLITE_LOCK_EXCLUSIVE && file->level == SQLITE_LOCK_RESERVED)
            file->level = SQLITE_LOCK_PENDING;
    }
    pthread_mutex_unlock(&registry);
    return result;
}

static int reserved_file(sqlite3_file *base, int *result) {
    struct native_file *file = (void *)base;
    *result = 0;
    pthread_mutex_lock(&registry);
    for (struct native_file *other = opened; other; other = other->next)
        if (!strcmp(other->path, file->path) && other->level >= SQLITE_LOCK_RESERVED) {
            *result = 1;
            break;
        }
    pthread_mutex_unlock(&registry);
    return SQLITE_OK;
}

static int control_file(sqlite3_file *base, int op, void *argument) {
    (void)base;
    (void)op;
    (void)argument;
    return SQLITE_NOTFOUND;
}

static int sector_size(sqlite3_file *file) {
    (void)file;
    return 512;
}

static int characteristics(sqlite3_file *file) {
    (void)file;
    return 0;
}

static int shared_map(sqlite3_file *base, int page, int size, int extend, void volatile **out) {
    struct native_file *file = (void *)base;
    *out = NULL;
    if (!file->shared || page < 0 || page >= 32 || size <= 0 || size > 65536)
        return SQLITE_IOERR_SHMMAP;
    pthread_mutex_lock(&registry);
    struct shared_index *index = file->shared;
    int result = SQLITE_OK;
    if (index->page_size && index->page_size != (unsigned)size) result = SQLITE_IOERR_SHMSIZE;
    else {
        index->page_size = size;
        if (!index->pages[page] && extend) {
            index->pages[page] = sqlite3_malloc(size);
            if (index->pages[page]) memset(index->pages[page], 0, size);
            else result = SQLITE_NOMEM;
        }
        *out = index->pages[page];
    }
    pthread_mutex_unlock(&registry);
    return result;
}

static int shared_lock(sqlite3_file *base, int offset, int count, int flags) {
    struct native_file *file = (void *)base;
    if (!file->shared || offset < 0 || count < 1 || offset + count > SQLITE_SHM_NLOCK)
        return SQLITE_IOERR_SHMLOCK;
    unsigned mask = ((1u << count) - 1) << offset;
    int result = SQLITE_OK;
    pthread_mutex_lock(&registry);
    if (flags & SQLITE_SHM_UNLOCK) {
        file->shared_locks &= ~mask;
        file->exclusive_locks &= ~mask;
    } else {
        for (struct native_file *other = opened; other; other = other->next) {
            if (other == file || other->shared != file->shared) continue;
            unsigned held = other->exclusive_locks;
            if (flags & SQLITE_SHM_EXCLUSIVE) held |= other->shared_locks;
            if (held & mask) {
                result = SQLITE_BUSY;
                break;
            }
        }
        if (result == SQLITE_OK) {
            if (flags & SQLITE_SHM_EXCLUSIVE) file->exclusive_locks |= mask;
            else file->shared_locks |= mask;
        }
    }
    pthread_mutex_unlock(&registry);
    return result;
}

static void shared_barrier(sqlite3_file *base) {
    (void)base;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static int shared_unmap(sqlite3_file *base, int remove) {
    (void)remove;
    struct native_file *file = (void *)base;
    pthread_mutex_lock(&registry);
    file->shared_locks = file->exclusive_locks = 0;
    pthread_mutex_unlock(&registry);
    return SQLITE_OK;
}

static const sqlite3_io_methods methods = {
    .iVersion = 2,
    .xClose = close_file,
    .xRead = read_file,
    .xWrite = write_file,
    .xTruncate = truncate_file,
    .xSync = sync_file,
    .xFileSize = size_file,
    .xLock = lock_file,
    .xUnlock = unlock_file,
    .xCheckReservedLock = reserved_file,
    .xFileControl = control_file,
    .xSectorSize = sector_size,
    .xDeviceCharacteristics = characteristics,
    .xShmMap = shared_map,
    .xShmLock = shared_lock,
    .xShmBarrier = shared_barrier,
    .xShmUnmap = shared_unmap,
};

static int open_file(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *base, int flags,
                     int *actual) {
    (void)vfs;
    struct native_file *file = (void *)base;
    memset(file, 0, sizeof(*file));
    int oflags = flags & SQLITE_OPEN_READWRITE ? O_RDWR : O_RDONLY;
    if (flags & SQLITE_OPEN_CREATE) oflags |= O_CREAT;
    if (flags & SQLITE_OPEN_EXCLUSIVE) oflags |= O_EXCL;
    if (name) {
        if (sos2_absolute_path(name, file->path)) return SQLITE_CANTOPEN;
    } else {
        pthread_mutex_lock(&registry);
        unsigned id = ++temporary_id;
        pthread_mutex_unlock(&registry);
        /* Pages share the browser profile's /tmp. */
        snprintf(file->path, sizeof(file->path), "/tmp/sqlite-%u-%u.tmp", (unsigned)getpid(), id);
        oflags = O_RDWR | O_CREAT | O_EXCL;
    }
    file->fd = open(file->path, oflags, 0600);
    if (file->fd < 0) return SQLITE_CANTOPEN;
    file->delete_on_close = !!(flags & SQLITE_OPEN_DELETEONCLOSE);
    /* Read-only resources never change; only /data and /tmp need the kernel lock. */
    file->database = (flags & SQLITE_OPEN_MAIN_DB) &&
                     (!strncmp(file->path, "/data/", 6) || !strncmp(file->path, "/tmp/", 5));
    pthread_mutex_lock(&registry);
    if (flags & SQLITE_OPEN_MAIN_DB) {
        for (struct native_file *other = opened; other; other = other->next)
            if (other->shared && !strcmp(other->path, file->path)) {
                file->shared = other->shared;
                break;
            }
        if (!file->shared) {
            file->shared = sqlite3_malloc(sizeof(*file->shared));
            if (file->shared) memset(file->shared, 0, sizeof(*file->shared));
        }
        if (!file->shared) {
            pthread_mutex_unlock(&registry);
            close(file->fd);
            return SQLITE_NOMEM;
        }
        ++file->shared->references;
    }
    file->base.pMethods = &methods;
    file->next = opened;
    opened = file;
    pthread_mutex_unlock(&registry);
    if (actual) *actual = flags;
    return SQLITE_OK;
}

static int delete_file(sqlite3_vfs *vfs, const char *name, int sync_dir) {
    (void)vfs;
    (void)sync_dir;
    /* littlefs commits and flushes directory changes in the unlink syscall. */
    return !unlink(name) || errno == ENOENT ? SQLITE_OK : SQLITE_IOERR_DELETE;
}

static int access_file(sqlite3_vfs *vfs, const char *name, int flags, int *result) {
    (void)vfs;
    int mode = flags == SQLITE_ACCESS_READWRITE ? R_OK | W_OK
               : flags == SQLITE_ACCESS_READ    ? R_OK
                                                : F_OK;
    *result = !access(name, mode);
    return SQLITE_OK;
}

static int full_path(sqlite3_vfs *vfs, const char *name, int size, char *out) {
    (void)vfs;
    char path[128];
    if (sos2_absolute_path(name, path) || (int)strlen(path) >= size) return SQLITE_CANTOPEN;
    strcpy(out, path);
    return SQLITE_OK;
}

static int randomness(sqlite3_vfs *vfs, int count, char *out) {
    (void)vfs;
    int done = 0;
    while (done < count) {
        int n = count - done;
        if (n > 256) n = 256;
        if (getentropy(out + done, n)) break;
        done += n;
    }
    return done;
}

static int sleep_for(sqlite3_vfs *vfs, int us) {
    (void)vfs;
    if (us < 0) return 0;
    struct timespec delay = {us / 1000000, (us % 1000000) * 1000L};
    return nanosleep(&delay, NULL) ? 0 : us;
}

static int time_now(sqlite3_vfs *vfs, sqlite3_int64 *out) {
    (void)vfs;
    struct timespec time;
    if (clock_gettime(CLOCK_REALTIME, &time)) return SQLITE_ERROR;
    *out = 210866760000000LL + (sqlite3_int64)time.tv_sec * 1000 + time.tv_nsec / 1000000;
    return SQLITE_OK;
}

static int julian_time(sqlite3_vfs *vfs, double *out) {
    sqlite3_int64 time;
    int result = time_now(vfs, &time);
    if (!result) *out = time / 86400000.0;
    return result;
}

static sqlite3_vfs vfs = {
    .iVersion = 2,
    .szOsFile = sizeof(struct native_file),
    .mxPathname = 127,
    .zName = "sos2",
    .xOpen = open_file,
    .xDelete = delete_file,
    .xAccess = access_file,
    .xFullPathname = full_path,
    .xRandomness = randomness,
    .xSleep = sleep_for,
    .xCurrentTime = julian_time,
    .xCurrentTimeInt64 = time_now,
};

int sqlite3_os_init(void) { return sqlite3_vfs_register(&vfs, 1); }

int sqlite3_os_end(void) { return sqlite3_vfs_unregister(&vfs); }
