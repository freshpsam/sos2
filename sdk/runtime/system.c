#include "internal.h"
#include <user_input.h>
#include <user_browser.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <dirent.h>
#include <dlfcn.h>
#include <poll.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sos2/device.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <malloc.h>
/* ARM's Newlib has O_BINARY (its sys/fcntl.h), x64's generic one none. */
#ifndef O_BINARY
#define O_BINARY 0
#endif

int sos2_errno(int e) {
    switch (e) {
#define ERROR(name) \
    case USER_##name: return name
        ERROR(ENOENT);
        ERROR(ESRCH);
        ERROR(EIO);
        ERROR(EBADF);
        ERROR(EAGAIN);
        ERROR(ENOMEM);
        ERROR(EFAULT);
        ERROR(EBUSY);
        ERROR(EEXIST);
        ERROR(EXDEV);
        ERROR(ENODEV);
        ERROR(ENOTDIR);
        ERROR(EISDIR);
        ERROR(EINVAL);
        ERROR(EMFILE);
        ERROR(EFBIG);
        ERROR(ENOSPC);
        ERROR(EROFS);
        ERROR(EDEADLK);
        ERROR(ENAMETOOLONG);
        ERROR(ENOSYS);
        ERROR(ENOTEMPTY);
        ERROR(ETIMEDOUT);
        ERROR(ENETDOWN);
        ERROR(ENETUNREACH);
        ERROR(ECONNABORTED);
        ERROR(ECONNRESET);
        ERROR(ECONNREFUSED);
        ERROR(EHOSTUNREACH);
#undef ERROR
        default: return e < 0 ? EIO : 0;
    }
}

int sos2_result(int result) {
    if (result < 0) {
        errno = sos2_errno(result);
        return -1;
    }
    return result;
}

int sos2_wait(uint32_t *word, uint32_t expected, uint32_t us) {
    return call3(SYS_WAIT_WORD, sos2_ptr(word), expected, us);
}

void sos2_wake(uint32_t *word, unsigned count) { call(SYS_WAKE_WORD, sos2_ptr(word), count); }

/* Monotonic time is the generic timer's virtual count, readable from user
 * mode (CNTKCTL.PL0VCTEN), converted as the kernel's time_us() converts it;
 * other clocks need the kernel. */
#ifdef __x86_64__
/* The counter's frequency, from r10 at entry (start.S). */
uint64_t sos2_counter_frequency;
#endif

static uint64_t monotonic_us(void) {
#ifdef __x86_64__
    /* The TSC, which user mode may read, shifted as the kernel's counter. */
    uint32_t low, high;
    __asm__ volatile("lfence\n rdtsc" : "=a"(low), "=d"(high));
    return user_timer_us((((uint64_t)high << 32) | low) >> USER_COUNTER_SHIFT,
                         (uint32_t)sos2_counter_frequency);
#elif defined(__aarch64__)
    uint64_t count, frequency;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(count));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    return user_timer_us(count, (uint32_t)frequency);
#else
    uint32_t low, high, frequency;
    __asm__ volatile("isb\n mrrc p15, 1, %0, %1, c14" : "=r"(low), "=r"(high));
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(frequency));
    return user_timer_us(((uint64_t)high << 32) | low, frequency);
#endif
}

int sos2_clock_read(int kind, uint64_t *out) {
    if (kind == 0) {
        *out = monotonic_us();
        return 0;
    }
    return call(SYS_CLOCK, kind, sos2_ptr(out));
}

uint64_t sos2_clock(int kind) {
    uint64_t value = 0;
    sos2_clock_read(kind, &value);
    return value;
}

int sos2_memory_info(struct user_memory_info *out) {
    return sos2_result(call(SYS_MEMORY_INFO, sos2_ptr(out), 0));
}

int sos2_present(const uint32_t *pixels, uint32_t bytes) {
    return sos2_result(call(SYS_PRESENT, sos2_ptr(pixels), bytes));
}

int sos2_present_damage(const uint32_t *pixels, unsigned x, unsigned y, unsigned width,
                        unsigned height) {
    struct user_present_damage damage = {sos2_ptr(pixels), x, y, width, height};
    return sos2_result(call(SYS_PRESENT_DAMAGE, sos2_ptr(&damage), sizeof(damage)));
}

int sos2_present_frame(const uint32_t *pixels, const struct user_pixel_scroll *scroll,
                       const struct user_pixel_rect *damage, unsigned count) {
    if (count > USER_FRAME_RECTS || (!damage && count)) return sos2_result(USER_EINVAL);
    struct user_present_frame frame = {.pixels = sos2_ptr(pixels), .count = count};
    if (scroll) frame.scroll = *scroll;
    for (unsigned i = 0; i < count; ++i) frame.damage[i] = damage[i];
    return sos2_result(call(SYS_PRESENT_FRAME, sos2_ptr(&frame), sizeof(frame)));
}

int sos2_surface_state(void) { return sos2_result(call(SYS_SURFACE_STATE, 0, 0)); }

int sos2_gpu(unsigned op, uintptr_t argument, uint32_t size) {
    return call3(SYS_GPU, op, argument, size);
}

#ifdef __aarch64__
/* EL0 cache maintenance (SCTLR_EL1.UCI and UCT). Clean and invalidate to the
 * point of coherency rather than cleaning to unification: early Cortex-A53
 * revisions need the stronger operation (errata 819472, 826319, 827319,
 * 824069), and both reach the instruction side. Broadcast to every CPU. */
void sos2_icache_sync(const void *code, size_t size) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t data = (uintptr_t)4 << ((ctr >> 16) & 15), instruction = (uintptr_t)4 << (ctr & 15);
    uintptr_t start = (uintptr_t)code, end = start + size;
    for (uintptr_t at = start & ~(data - 1); at < end; at += data)
        __asm__ volatile("dc civac, %0" : : "r"(at) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (uintptr_t at = start & ~(instruction - 1); at < end; at += instruction)
        __asm__ volatile("ic ivau, %0" : : "r"(at) : "memory");
    __asm__ volatile("dsb ish\n isb" ::: "memory");
}
#elif defined(__x86_64__)
/* x86's instruction fetch sees stores by itself, on every CPU. */
void sos2_icache_sync(const void *code, size_t size) {
    (void)code;
    (void)size;
}
#else
/* AArch32 user mode cannot maintain caches: the kernel does it on this CPU
 * (SYS_ICACHE_SYNC), at most 256 KiB at a time. */
void sos2_icache_sync(const void *code, size_t size) {
    uintptr_t at = (uintptr_t)code, end = at + size;
    while (at < end) {
        size_t n = end - at > 0x40000 ? 0x40000 : end - at;
        if (call(SYS_ICACHE_SYNC, at, n)) abort();
        at += n;
    }
}
#endif

int sos2_next_event(struct user_input_event *event) {
    return sos2_result(call(SYS_NEXT_EVENT, sos2_ptr(event), sizeof(*event)));
}

int sos2_input_method(int enabled) {
    return sos2_result(call(SYS_INPUT_METHOD, (unsigned)enabled, 0));
}

int sos2_back_key(int enabled) { return sos2_result(call(SYS_BACK_KEY, (unsigned)enabled, 0)); }

int sos2_haptic_tap(void) { return sos2_result(call(SYS_HAPTIC_TAP, 0, 0)); }

int sos2_update(unsigned op, void *data, uint32_t size) {
    return call3(SYS_UPDATE, op, sos2_ptr(data), size);
}

int sos2_audio(unsigned op, uintptr_t argument, uint32_t count) {
    return call3(SYS_AUDIO, op, argument, count);
}

int sos2_browser_command(struct user_browser_command *out) {
    return sos2_result(call(SYS_BROWSER_COMMAND, sos2_ptr(out), sizeof(*out)));
}

int sos2_browser_state(const struct user_browser_state *state) {
    return sos2_result(call(SYS_BROWSER_STATE, sos2_ptr(state), sizeof(*state)));
}

void _exit(int status) { finish(status); }

void abort(void) { finish(134); }

int _getpid(void) { return call(SYS_GETPID, 0, 0); }

int _kill(int pid, int signal) {
    (void)pid;
    (void)signal;
    errno = ENOSYS;
    return -1;
}

int _link(const char *old, const char *new) {
    (void)old;
    (void)new;
    errno = ENOTSUP;
    return -1;
}

/* Newlib malloc owns this contiguous break; other native mappings cannot replace it. */
void *_sbrk(ptrdiff_t change) {
    static uintptr_t end = SOS2_BRK_BASE, mapped = SOS2_BRK_BASE;
    uintptr_t old = end;
    if (change < 0 || (uintptr_t)change > USER_HEAP_END - end) {
        errno = ENOMEM;
        return (void *)-1;
    }
    uintptr_t next = end + (uintptr_t)change, rounded = (next + 4095) & ~(uintptr_t)4095;
    if (rounded > mapped) {
        /* Success returns the address, so check the full-width status. */
        sos2_status r = call3(SYS_VM_MAP, mapped, (rounded - mapped) / 4096, 3);
        if (r < 0) {
            errno = sos2_errno((int)r);
            return (void *)-1;
        }
    }
    end = next;
    mapped = rounded;
    return (void *)old;
}

static struct sos2_lock files_lock;
static char working_directory[128] = "/";

static struct {
    unsigned used, flags, offset;
} files[USER_FILE_HANDLES];

#define DESCRIPTORS (USER_FILE_HANDLES + 3)
#define CONSOLE_IN  (USER_FILE_HANDLES + 1)
#define CONSOLE_OUT (USER_FILE_HANDLES + 2)
#define CONSOLE_ERR (USER_FILE_HANDLES + 3)
/* libc descriptors reference open descriptions, whose offsets are shared by
 * dup. Kernel handles remain live until their last libc descriptor closes. */
static unsigned descriptors[DESCRIPTORS] = {CONSOLE_IN, CONSOLE_OUT, CONSOLE_ERR};
static unsigned descriptor_flags[DESCRIPTORS];
/* User-space devices (sos2/device.h): their open descriptions are numbered
 * after the console's. A description holds one reference per descriptor and
 * one per call in progress, so release never runs under a device operation. */
#define DEVICE_PATHS 4
#define DEVICE_OPENS 16
#define DEVICE_FIRST (USER_FILE_HANDLES + 4)
static const struct sos2_device *device_paths[DEVICE_PATHS];

static struct {
    const struct sos2_device *device;
    void *state;
    unsigned used;
} device_opens[DEVICE_OPENS];

static uint32_t devices_open;

static int device_index(unsigned item) {
    return item >= DEVICE_FIRST && item < DEVICE_FIRST + DEVICE_OPENS ? (int)(item - DEVICE_FIRST)
                                                                      : -1;
}

static void device_put(int index) {
    if (--device_opens[index].used) return;
    const struct sos2_device *device = device_opens[index].device;
    void *state = device_opens[index].state;
    device_opens[index].device = NULL;
    device_opens[index].state = NULL;
    sos2_store(&devices_open, devices_open - 1);
    if (device->release) device->release(state);
}

/* Only canonical names cross the native ABI. Relative paths are a libc feature. */
static int canonical(const char *path, char out[128]) {
    if (!path || !*path) {
        errno = ENOENT;
        return -1;
    }
    char input[256];
    size_t length = strnlen(path, 256);
    if (length == 256) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (path[0] == '/') memcpy(input, path, length + 1);
    else {
        size_t cwd = strlen(working_directory);
        if (cwd + 1 + length >= sizeof(input)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(input, working_directory, cwd);
        input[cwd++] = '/';
        memcpy(input + cwd, path, length + 1);
    }
    unsigned used = 1;
    out[0] = '/';
    for (const char *p = input; *p;) {
        while (*p == '/') ++p;
        const char *first = p;
        while (*p && *p != '/') ++p;
        unsigned n = p - first;
        if (!n || (n == 1 && first[0] == '.')) continue;
        if (n == 2 && first[0] == '.' && first[1] == '.') {
            while (used > 1 && out[--used] != '/') { }
            continue;
        }
        if (used > 1) {
            if (used == 127) {
                errno = ENAMETOOLONG;
                return -1;
            }
            out[used++] = '/';
        }
        if (n > 127 - used) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(out + used, first, n);
        used += n;
    }
    out[used] = 0;
    return 0;
}

static unsigned description(int fd) {
    if (fd < 0 || fd >= (int)DESCRIPTORS || !descriptors[fd]) {
        errno = EBADF;
        return 0;
    }
    return descriptors[fd];
}

static int index_of(int fd) {
    unsigned item = description(fd);
    if (!item || item > USER_FILE_HANDLES) {
        errno = EBADF;
        return -1;
    }
    return item - 1;
}

static int vacant(int first) {
    if (first < 0 || first >= (int)DESCRIPTORS) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned i = first; i < DESCRIPTORS; ++i)
        if (!descriptors[i]) return i;
    errno = EMFILE;
    return -1;
}

static int close_locked(int fd) {
    unsigned item = description(fd);
    if (!item) return -1;
    descriptors[fd] = descriptor_flags[fd] = 0;
    if (item <= USER_FILE_HANDLES && !--files[item - 1].used)
        return sos2_result(call(SYS_FILE_CLOSE, item, 0));
    int device = device_index(item);
    if (device >= 0) device_put(device);
    return 0;
}

/* The device behind a descriptor, with a reference held: -1 (errno EBADF)
 * for a descriptor that is not open, -2 for one that is not a device. */
static int device_get(int fd) {
    sos2_lock(&files_lock);
    unsigned item = description(fd);
    int index = item ? device_index(item) : -1;
    if (index >= 0) ++device_opens[index].used;
    else if (item) index = -2;
    sos2_unlock(&files_lock);
    return index;
}

static void device_unget(int index) {
    int error = errno;
    sos2_lock(&files_lock);
    device_put(index);
    sos2_unlock(&files_lock);
    errno = error;
}

int sos2_device_register(const struct sos2_device *device) {
    sos2_lock(&files_lock);
    int error = ENOMEM;
    unsigned free = DEVICE_PATHS;
    for (unsigned i = 0; i < DEVICE_PATHS; ++i) {
        if (!device_paths[i]) {
            if (free == DEVICE_PATHS) free = i;
        } else if (!strcmp(device_paths[i]->path, device->path)) error = EEXIST;
    }
    if (error != EEXIST && free < DEVICE_PATHS) {
        device_paths[free] = device;
        error = 0;
    }
    sos2_unlock(&files_lock);
    if (error) errno = error;
    return error ? -1 : 0;
}

/* Caller holds files_lock; state NULL asks the device to open one. */
static int open_device(const struct sos2_device *device, void *state, int fd, int flags) {
    for (unsigned i = 0; i < DEVICE_OPENS; ++i)
        if (!device_opens[i].used) {
            if (!state && !(state = device->open(flags))) return -1;
            device_opens[i].device = device;
            device_opens[i].state = state;
            device_opens[i].used = 1;
            sos2_store(&devices_open, devices_open + 1);
            descriptors[fd] = DEVICE_FIRST + i;
            descriptor_flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
            return fd;
        }
    errno = ENFILE;
    return -1;
}

int sos2_device_open_state(const struct sos2_device *device, void *state, int flags) {
    if (!device || !state) {
        errno = EINVAL;
        return -1;
    }
    sos2_lock(&files_lock);
    int fd = vacant(0), result = fd < 0 ? -1 : open_device(device, state, fd, flags);
    sos2_unlock(&files_lock);
    return result;
}

void *sos2_device_state(int fd, const struct sos2_device *device) {
    sos2_lock(&files_lock);
    unsigned item = description(fd);
    int index = item ? device_index(item) : -1;
    void *state = NULL;
    if (index >= 0 && device_opens[index].device == device) state = device_opens[index].state;
    else if (item) errno = ENODEV;
    sos2_unlock(&files_lock);
    return state;
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *argument = va_arg(ap, void *);
    va_end(ap);
    int index = device_get(fd);
    if (index < 0) {
        if (index == -2) errno = ENOTTY;
        return -1;
    }
    const struct sos2_device *device = device_opens[index].device;
    int result = -1;
    if (device->ioctl) result = device->ioctl(device_opens[index].state, request, argument);
    else errno = ENOTTY;
    device_unget(index);
    return result;
}

static int duplicate(int from, int minimum, int target, unsigned flags) {
    sos2_lock(&files_lock);
    unsigned item = description(from);
    int result = -1;
    if (item) {
        if (target >= 0 && target >= (int)DESCRIPTORS) errno = EBADF;
        else if (target == from) result = from;
        else {
            int fd = target >= 0 ? target : vacant(minimum);
            if (fd >= 0) {
                if (descriptors[fd]) close_locked(fd);
                descriptors[fd] = item;
                descriptor_flags[fd] = flags;
                if (item <= USER_FILE_HANDLES) ++files[item - 1].used;
                else if (device_index(item) >= 0) ++device_opens[device_index(item)].used;
                result = fd;
            }
        }
    }
    sos2_unlock(&files_lock);
    return result;
}

int dup(int fd) { return duplicate(fd, 0, -1, 0); }

ssize_t pread(int fd, void *buffer, size_t size, off_t offset) {
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    sos2_lock(&files_lock);
    off_t saved = lseek(fd, 0, SEEK_CUR);
    ssize_t result = -1;
    if (saved >= 0 && lseek(fd, offset, SEEK_SET) >= 0) {
        result = read(fd, buffer, size);
        int error = errno;
        if (lseek(fd, saved, SEEK_SET) < 0) result = -1;
        else errno = error;
    }
    sos2_unlock(&files_lock);
    return result;
}

int dup2(int from, int to) {
    if (to < 0) {
        errno = EBADF;
        return -1;
    }
    return duplicate(from, 0, to, 0);
}

int fcntl(int fd, int operation, ...) {
    if (operation == F_DUPFD || operation == F_DUPFD_CLOEXEC) {
        va_list ap;
        va_start(ap, operation);
        int minimum = va_arg(ap, int);
        va_end(ap);
        return duplicate(fd, minimum, -1, operation == F_DUPFD_CLOEXEC ? FD_CLOEXEC : 0);
    }
    sos2_lock(&files_lock);
    unsigned item = description(fd);
    int result = -1;
    if (item) {
        if (operation == F_GETFD) result = descriptor_flags[fd];
        else if (operation == F_SETFD) {
            va_list ap;
            va_start(ap, operation);
            int flags = va_arg(ap, int);
            va_end(ap);
            if (flags & ~FD_CLOEXEC) errno = EINVAL;
            else result = 0, descriptor_flags[fd] = flags;
        } else if (operation == F_GETFL) {
            unsigned flags = device_index(item) >= 0 ? FILE_READ | FILE_WRITE
                             : item > USER_FILE_HANDLES
                                 ? (item == CONSOLE_IN ? FILE_READ : FILE_WRITE)
                                 : files[item - 1].flags;
            result = (flags & FILE_WRITE) ? ((flags & FILE_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
            if (flags & FILE_APPEND) result |= O_APPEND;
        } else errno = ENOTSUP;
    }
    sos2_unlock(&files_lock);
    return result;
}

int sos2_absolute_path(const char *path, char out[128]) {
    sos2_lock(&files_lock);
    int result = canonical(path, out);
    sos2_unlock(&files_lock);
    return result;
}

int sos2_file_lock(const char *path, int locked) {
    char name[128];
    if (sos2_absolute_path(path, name)) return -1;
    return sos2_result(call3(SYS_FILE_LOCK, sos2_ptr(name), strlen(name), (unsigned)locked));
}

int _open(const char *path, int flags, ...) {
    unsigned native;
    switch (flags & O_ACCMODE) {
        case O_RDONLY: native = FILE_READ; break;
        case O_WRONLY: native = FILE_WRITE; break;
        case O_RDWR: native = FILE_READ | FILE_WRITE; break;
        default: errno = EINVAL; return -1;
    }
    if (flags &
        ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_CLOEXEC | O_DIRECTORY | O_BINARY)) {
        errno = ENOTSUP;
        return -1;
    }
    if (flags & O_CREAT) native |= FILE_CREATE;
    if (flags & O_EXCL) native |= FILE_EXCLUSIVE;
    if (flags & O_TRUNC) native |= FILE_TRUNCATE;
    if (flags & O_APPEND) native |= FILE_APPEND;
    if (flags & O_DIRECTORY) native |= FILE_DIRECTORY;
    sos2_lock(&files_lock);
    char name[128];
    int result = -1;
    int fd = vacant(0), named = fd >= 0 && !canonical(path, name);
    const struct sos2_device *device = NULL;
    for (unsigned i = 0; named && i < DEVICE_PATHS; ++i)
        if (device_paths[i] && !strcmp(device_paths[i]->path, name)) device = device_paths[i];
    if (device) result = open_device(device, NULL, fd, flags);
    else if (named) {
        int handle = sos2_result(call3(SYS_FILE_OPEN, sos2_ptr(name), strlen(name), native));
        if (handle > 0) {
            files[handle - 1].used = 1;
            files[handle - 1].flags = native;
            files[handle - 1].offset = 0;
            descriptors[fd] = handle;
            descriptor_flags[fd] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
            result = fd;
        }
    }
    sos2_unlock(&files_lock);
    return result;
}

int _close(int fd) {
    sos2_lock(&files_lock);
    int result = close_locked(fd);
    sos2_unlock(&files_lock);
    return result;
}

static int transfer(int fd, void *buffer, size_t length, int writing) {
    if (length > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    sos2_lock(&files_lock);
    unsigned item = description(fd);
    if (!item) {
        sos2_unlock(&files_lock);
        return -1;
    }
    if (!writing && item == CONSOLE_IN) {
        sos2_unlock(&files_lock);
        return 0;
    }
    /* The console has no output: what programs print goes nowhere. */
    if (writing && (item == CONSOLE_OUT || item == CONSOLE_ERR)) {
        sos2_unlock(&files_lock);
        return length;
    }
    int index = index_of(fd);
    size_t total = 0;
    int r = -1;
    if (index >= 0) {
        r = 0;
        while (total < length) {
            unsigned n = length - total;
            if (n > USER_FILE_READ_MAX) n = USER_FILE_READ_MAX;
            r = sos2_result(call3(writing ? SYS_FILE_WRITE : SYS_FILE_READ, index + 1,
                                  sos2_ptr((char *)buffer + total), n));
            if (r <= 0) break;
            total += r;
            files[index].offset += r;
            if ((unsigned)r < n) break;
        }
        if (writing && (files[index].flags & FILE_APPEND)) {
            int size = call(SYS_FILE_SIZE, index + 1, 0);
            if (size >= 0) files[index].offset = size;
        }
    }
    sos2_unlock(&files_lock);
    return total ? (int)total : r;
}

int _read(int fd, void *buf, size_t length) { return transfer(fd, buf, length, 0); }

int _write(int fd, const void *buf, size_t length) { return transfer(fd, (void *)buf, length, 1); }

off_t _lseek(int fd, off_t offset, int whence) {
    sos2_lock(&files_lock);
    int index = index_of(fd), result = -1;
    if (index >= 0) {
        int64_t base = 0;
        if (whence == SEEK_CUR) base = files[index].offset;
        else if (whence == SEEK_END) base = sos2_result(call(SYS_FILE_SIZE, index + 1, 0));
        else if (whence != SEEK_SET) {
            errno = EINVAL;
            base = -1;
        }
        int64_t next = base + offset;
        if (base >= 0 && next >= 0 && next <= INT_MAX && next == files[index].offset &&
            whence == SEEK_CUR)
            result = next;
        else if (base >= 0 && next >= 0 && next <= INT_MAX) {
            result = sos2_result(call(SYS_FILE_SEEK, index + 1, next));
            if (result >= 0) {
                files[index].offset = next;
                result = next;
            }
        } else if (base >= 0) errno = EINVAL;
    }
    sos2_unlock(&files_lock);
    return result;
}

static void fill_stat(struct stat *out, const struct user_file_info *info) {
    memset(out, 0, sizeof(*out));
    out->st_mode = (info->type == FILE_DIR ? S_IFDIR : S_IFREG) | 0600;
    if (info->type == FILE_DIR) out->st_mode |= 0100;
    out->st_size = info->size;
    out->st_nlink = 1;
    out->st_blksize = 4096;
    out->st_blocks = (info->size + 511) / 512;
}

int _fstat(int fd, struct stat *out) {
    sos2_lock(&files_lock);
    unsigned item = description(fd);
    if (item > USER_FILE_HANDLES) {
        memset(out, 0, sizeof(*out));
        out->st_mode = S_IFCHR | 0600;
        sos2_unlock(&files_lock);
        return 0;
    }
    int index = index_of(fd), r = -1;
    struct user_file_info info;
    if (index >= 0) {
        r = sos2_result(call(SYS_FILE_INFO, index + 1, sos2_ptr(&info)));
        if (!r) fill_stat(out, &info);
    }
    sos2_unlock(&files_lock);
    return r;
}

int _stat(const char *path, struct stat *out) {
    sos2_lock(&files_lock);
    char name[128];
    int r = canonical(path, name);
    struct user_file_info info;
    if (!r) {
        r = sos2_result(call3(SYS_PATH_INFO, sos2_ptr(name), strlen(name), sos2_ptr(&info)));
        if (!r) fill_stat(out, &info);
    }
    sos2_unlock(&files_lock);
    return r;
}

int lstat(const char *path, struct stat *out) { return _stat(path, out); }

int statvfs(const char *path, struct statvfs *out) {
    sos2_lock(&files_lock);
    char name[128];
    int r = canonical(path, name);
    struct user_fs_info info;
    if (!r) r = sos2_result(call3(SYS_FS_INFO, sos2_ptr(name), strlen(name), sos2_ptr(&info)));
    if (!r) {
        memset(out, 0, sizeof(*out));
        out->f_bsize = out->f_frsize = info.block_size;
        out->f_blocks = info.blocks;
        out->f_bfree = out->f_bavail = info.free_blocks;
        out->f_namemax = info.name_max;
        out->f_flag = ST_NOSUID | (info.read_only ? ST_RDONLY : 0);
    }
    sos2_unlock(&files_lock);
    return r;
}

char *realpath(const char *path, char *out) {
    sos2_lock(&files_lock);
    char name[128];
    struct user_file_info info;
    int r = canonical(path, name);
    if (!r) r = sos2_result(call3(SYS_PATH_INFO, sos2_ptr(name), strlen(name), sos2_ptr(&info)));
    if (!r) {
        if (!out) out = malloc(strlen(name) + 1);
        if (out) strcpy(out, name);
    }
    sos2_unlock(&files_lock);
    return r ? NULL : out;
}

int _isatty(int fd) {
    sos2_lock(&files_lock);
    unsigned item = description(fd);
    sos2_unlock(&files_lock);
    if (item == CONSOLE_IN || item == CONSOLE_OUT || item == CONSOLE_ERR) return 1;
    if (item) errno = ENOTTY;
    return 0;
}

int fsync(int fd) {
    sos2_lock(&files_lock);
    int i = index_of(fd);
    int r = i < 0 ? -1 : sos2_result(call(SYS_FILE_SYNC, i + 1, 0));
    sos2_unlock(&files_lock);
    return r;
}

int ftruncate(int fd, off_t length) {
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }
    sos2_lock(&files_lock);
    int i = index_of(fd);
    int r = i < 0 ? -1 : sos2_result(call(SYS_FILE_TRUNCATE, i + 1, length));
    sos2_unlock(&files_lock);
    return r;
}

int mkdir(const char *path, mode_t mode) {
    (void)mode;
    sos2_lock(&files_lock);
    char name[128];
    int r = canonical(path, name);
    if (!r) r = sos2_result(call(SYS_PATH_MKDIR, sos2_ptr(name), strlen(name)));
    sos2_unlock(&files_lock);
    return r;
}

static int remove_path(const char *path, int directory) {
    sos2_lock(&files_lock);
    char name[128];
    int r = canonical(path, name);
    struct user_file_info info;
    if (!r) r = sos2_result(call3(SYS_PATH_INFO, sos2_ptr(name), strlen(name), sos2_ptr(&info)));
    if (!r && (info.type == FILE_DIR) != directory) {
        errno = directory ? ENOTDIR : EISDIR;
        r = -1;
    }
    if (!r) r = sos2_result(call(SYS_PATH_REMOVE, sos2_ptr(name), strlen(name)));
    sos2_unlock(&files_lock);
    return r;
}

int _unlink(const char *path) { return remove_path(path, 0); }

int rmdir(const char *path) { return remove_path(path, 1); }

int _rename(const char *old, const char *new) {
    sos2_lock(&files_lock);
    char a[128], b[128];
    int r = canonical(old, a);
    if (!r) r = canonical(new, b);
    if (!r) r = sos2_result(call4(SYS_PATH_RENAME, sos2_ptr(a), strlen(a), sos2_ptr(b), strlen(b)));
    sos2_unlock(&files_lock);
    return r;
}

int rename(const char *old, const char *new) { return _rename(old, new); }

int chdir(const char *path) {
    sos2_lock(&files_lock);
    char name[128];
    int r = canonical(path, name);
    struct user_file_info info;
    if (!r) r = sos2_result(call3(SYS_PATH_INFO, sos2_ptr(name), strlen(name), sos2_ptr(&info)));
    if (!r && info.type != FILE_DIR) {
        errno = ENOTDIR;
        r = -1;
    }
    if (!r) strcpy(working_directory, name);
    sos2_unlock(&files_lock);
    return r;
}

char *getcwd(char *out, size_t size) {
    sos2_lock(&files_lock);
    size_t need = strlen(working_directory) + 1;
    int allocated = !out;
    if (allocated) {
        if (!size) size = need;
        out = malloc(size);
    }
    if (!out) errno = ENOMEM;
    else if (size < need) {
        if (allocated) free(out);
        errno = ERANGE;
        out = NULL;
    } else memcpy(out, working_directory, need);
    sos2_unlock(&files_lock);
    return out;
}

int access(const char *path, int mode) {
    struct stat st;
    if (_stat(path, &st)) return -1;
    if (mode & ~(R_OK | W_OK | X_OK)) {
        errno = EINVAL;
        return -1;
    }
    if ((mode & X_OK) && !S_ISDIR(st.st_mode)) {
        errno = EACCES;
        return -1;
    }
    if (mode & W_OK) {
        char name[128];
        sos2_lock(&files_lock);
        int r = canonical(path, name);
        sos2_unlock(&files_lock);
        if (r) return -1;
        int data = !strncmp(name, "/data", 5) && (!name[5] || name[5] == '/');
        int temp = !strncmp(name, "/tmp", 4) && (!name[4] || name[4] == '/');
        if (!data && !temp) {
            errno = EROFS;
            return -1;
        }
    }
    return 0;
}

struct sos2_directory {
    int fd;
    struct dirent entry;
};

DIR *opendir(const char *path) {
    int fd = _open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return NULL;
    DIR *dir = calloc(1, sizeof(*dir));
    if (!dir) {
        _close(fd);
        return NULL;
    }
    dir->fd = fd;
    return dir;
}

struct dirent *readdir(DIR *dir) {
    struct user_dir_entry entry;
    sos2_lock(&files_lock);
    int i = index_of(dir->fd);
    int r = i < 0 ? -1 : sos2_result(call(SYS_FILE_NEXT, i + 1, sos2_ptr(&entry)));
    sos2_unlock(&files_lock);
    if (r <= 0) return NULL;
    dir->entry.d_ino++;
    dir->entry.d_type = entry.type == FILE_DIR ? DT_DIR : DT_REG;
    strcpy(dir->entry.d_name, entry.name);
    return &dir->entry;
}

int closedir(DIR *dir) {
    int r = _close(dir->fd);
    free(dir);
    return r;
}

int clock_gettime(clockid_t clock, struct timespec *out) {
    int kind;
    switch (clock) {
        case CLOCK_MONOTONIC: kind = 0; break;
        case CLOCK_REALTIME: kind = 1; break;
        case CLOCK_PROCESS_CPUTIME_ID: kind = 2; break;
        case CLOCK_THREAD_CPUTIME_ID: kind = 3; break;
        default: errno = EINVAL; return -1;
    }
    if (!out) {
        errno = EFAULT;
        return -1;
    }
    uint64_t value;
    int status = sos2_clock_read(kind, &value);
    /* Callers such as libstdc++'s system_clock::now() ignore the result;
     * never leave their stack value behind when wall time is unavailable. */
    if (status < 0) {
        *out = (struct timespec){0};
        return sos2_result(status);
    }
    out->tv_sec = value / 1000000;
    out->tv_nsec = (value % 1000000) * 1000;
    return 0;
}

int clock_getres(clockid_t clock, struct timespec *out) {
    if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME && clock != CLOCK_PROCESS_CPUTIME_ID &&
        clock != CLOCK_THREAD_CPUTIME_ID) {
        errno = EINVAL;
        return -1;
    }
    if (out) *out = (struct timespec){0, 1000};
    return 0;
}

int _gettimeofday(struct timeval *out, void *zone) {
    (void)zone;
    if (!out) {
        errno = EFAULT;
        return -1;
    }
    uint64_t value;
    int status = sos2_clock_read(1, &value);
    if (status < 0) {
        *out = (struct timeval){0};
        return sos2_result(status);
    }
    out->tv_sec = value / 1000000;
    out->tv_usec = value % 1000000;
    return 0;
}

int getrusage(int who, struct rusage *usage) {
    if (who != RUSAGE_SELF && who != RUSAGE_THREAD) {
        errno = ENOTSUP;
        return -1;
    }
    uint64_t us = sos2_clock(who == RUSAGE_SELF ? 2 : 3);
    memset(usage, 0, sizeof(*usage));
    usage->ru_utime.tv_sec = us / 1000000;
    usage->ru_utime.tv_usec = us % 1000000;
    return 0;
}

clock_t _times(struct tms *times) {
    memset(times, 0, sizeof(*times));
    times->tms_utime = sos2_clock(2) * CLOCKS_PER_SEC / 1000000;
    return sos2_clock(0) * CLOCKS_PER_SEC / 1000000;
}

int nanosleep(const struct timespec *duration, struct timespec *remaining) {
    if (duration->tv_sec < 0 || duration->tv_nsec < 0 || duration->tv_nsec >= 1000000000) {
        errno = EINVAL;
        return -1;
    }
    uint64_t us = (uint64_t)duration->tv_sec * 1000000 + (duration->tv_nsec + 999) / 1000;
    while (us) {
        unsigned n = us > 1000000 ? 1000000 : us;
        call(SYS_SLEEP, n, 0);
        us -= n;
    }
    if (remaining) *remaining = (struct timespec){0, 0};
    return 0;
}

/* One user, no symbolic links, no signals. */
uid_t getuid(void) { return 0; }

uid_t geteuid(void) { return 0; }

gid_t getgid(void) { return 0; }

gid_t getegid(void) { return 0; }

int getpagesize(void) { return 4096; }

ssize_t readlink(const char *path, char *buffer, size_t size) {
    (void)buffer;
    (void)size;
    struct stat s;
    if (!_stat(path, &s)) errno = EINVAL;
    return -1;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old) {
    if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) return EINVAL;
    (void)set;
    if (old) sigemptyset(old);
    return 0;
}

void sync(void) {
    sos2_lock(&files_lock);
    for (unsigned i = 0; i < USER_FILE_HANDLES; ++i)
        if (files[i].used) (void)call(SYS_FILE_SYNC, i + 1, 0);
    sos2_unlock(&files_lock);
}

/* Programs with their own allocator (the browser's mimalloc) replace this. */
__attribute__((weak)) int posix_memalign(void **out, size_t alignment, size_t size) {
    if (alignment < sizeof(void *) || (alignment & (alignment - 1))) return EINVAL;
    void *p = memalign(alignment, size);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}

int usleep(useconds_t us) {
    struct timespec d = {us / 1000000, (long)(us % 1000000) * 1000};
    return nanosleep(&d, NULL);
}

unsigned sleep(unsigned seconds) {
    struct timespec d = {seconds, 0};
    nanosleep(&d, NULL);
    return 0;
}

int getentropy(void *buffer, size_t length) {
    if (length > 256) {
        errno = EIO;
        return -1;
    }
    for (size_t done = 0; done < length;) {
        int n = call(SYS_RANDOM, sos2_ptr((char *)buffer + done), length - done);
        if (n == USER_EAGAIN) {
            call(SYS_YIELD, 0, 0);
            continue;
        }
        if (sos2_result(n) < 0) return -1;
        done += n;
    }
    return 0;
}

long sysconf(int name) {
    switch (name) {
        case _SC_PAGESIZE: return 4096;
        case _SC_NPROCESSORS_ONLN:
        case _SC_NPROCESSORS_CONF: return sos2_result(call(SYS_CPU_COUNT, 0, 0));
        case _SC_OPEN_MAX: return USER_FILE_HANDLES + 3;
        case _SC_THREAD_THREADS_MAX: return USER_THREAD_LIMIT;
        case _SC_THREAD_STACK_MIN: return 4096;
        case _SC_THREAD_KEYS_MAX: return SOS2_KEYS;
        case _SC_CLK_TCK: return CLOCKS_PER_SEC;
        case _SC_PHYS_PAGES:
        case _SC_AVPHYS_PAGES: {
            struct user_memory_info info;
            if (sos2_memory_info(&info)) return -1;
            return name == _SC_PHYS_PAGES ? info.quota_pages : info.available_pages;
        }
        default: errno = EINVAL; return -1;
    }
}

int sched_yield(void) { return sos2_result(call(SYS_YIELD, 0, 0)); }

/* The longest mapping: the heap's span (include/user_abi.h). */
#define MAPPING_MAX (USER_HEAP_END - USER_HEAP_BASE)

void *mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset) {
    /* MAP_ALIGNED(n), for anonymous memory only: the kernel places it at a
     * multiple of 2^n bytes, with no slack to unmap afterwards. */
    unsigned alignment = ((unsigned)flags & (unsigned)MAP_ALIGNMENT_MASK) >> MAP_ALIGNMENT_SHIFT;
    flags &= ~MAP_ALIGNMENT_MASK;
    if (alignment &&
        (!(flags & MAP_ANONYMOUS) || alignment < 12 || alignment - 12 > USER_MAP_ALIGN_MAX)) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    unsigned align_pages = alignment ? alignment - 12 : 0;
    if (!(flags & MAP_ANONYMOUS)) {
        int index = device_get(fd);
        if (index == -1) return MAP_FAILED;
        if (index >= 0) {
            const struct sos2_device *device = device_opens[index].device;
            void *result = MAP_FAILED;
            if (device->mmap)
                result = device->mmap(device_opens[index].state, length, prot, flags, offset);
            else errno = ENODEV;
            device_unget(index);
            return result;
        }
    }
    if (!(flags & MAP_PRIVATE) || (flags & ~(MAP_PRIVATE | MAP_ANONYMOUS))) {
        errno = ENOTSUP;
        return MAP_FAILED;
    }
    int file = !(flags & MAP_ANONYMOUS);
    /* Writable executable memory lives in the JIT window, which the kernel
     * grants only to the engine. */
    int jit = (prot & 6) == 6 && !file;
    if (!length || length > MAPPING_MAX || (prot & ~7) || ((prot & 6) == 6 && !jit) ||
        (prot && !(prot & PROT_READ)) || offset < 0 || (offset & 4095)) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    unsigned pages = (length + 4095) / 4096;
    int permissions = file ? PROT_READ | PROT_WRITE : prot;
    /* address is a hint, not authorization to replace existing allocations. */
    sos2_word hint = sos2_ptr(address);
    sos2_status r = USER_EINVAL;
    unsigned placed = permissions | align_pages << USER_MAP_ALIGN_SHIFT;
    if (jit) r = call4(SYS_VM_ALLOC_RANGE, USER_JIT_BASE, USER_JIT_END, pages, placed);
    if (!jit && hint >= USER_HEAP_BASE && hint < SOS2_BRK_BASE &&
        pages <= (SOS2_BRK_BASE - hint) / 4096 && !(hint & ((4096u << align_pages) - 1)))
        r = call3(SYS_VM_MAP, hint, pages, permissions);
    if (r < 0 && !jit) r = sos2_map(pages, placed);
    if (r < 0) {
        errno = sos2_errno((int)r);
        return MAP_FAILED;
    }
    void *mapped = (void *)(uintptr_t)r;
    if (file) {
        sos2_lock(&files_lock);
        int index = index_of(fd), error = index < 0 ? EBADF : 0;
        if (!error && !(files[index].flags & FILE_READ)) error = EACCES;
        int size = error ? -1 : call(SYS_FILE_SIZE, index + 1, 0);
        if (!error && size < 0) error = sos2_errno(size);
        if (!error && (offset >= size || length > (((unsigned)(size - offset) + 4095u) & ~4095u)))
            error = EINVAL;
        if (!error) {
            r = call(SYS_FILE_SEEK, index + 1, offset);
            if (r < 0) error = sos2_errno(r);
            size_t count = size - offset;
            if (count > length) count = length;
            for (size_t done = 0; !error && done < count;) {
                unsigned chunk = count - done;
                if (chunk > USER_FILE_READ_MAX) chunk = USER_FILE_READ_MAX;
                r = call3(SYS_FILE_READ, index + 1, sos2_ptr((char *)mapped + done), chunk);
                if (r <= 0) error = r < 0 ? sos2_errno(r) : EIO;
                else done += r;
            }
            r = call(SYS_FILE_SEEK, index + 1, files[index].offset);
            if (r < 0 && !error) error = sos2_errno(r);
        }
        sos2_unlock(&files_lock);
        if (!error) {
            r = call3(SYS_VM_PROTECT, sos2_ptr(mapped), pages, prot);
            if (r < 0) error = sos2_errno(r);
        }
        if (error) {
            call3(SYS_VM_UNMAP, sos2_ptr(mapped), pages, 0);
            errno = error;
            return MAP_FAILED;
        }
    }
    return mapped;
}

static int memory_op(unsigned op, void *address, size_t length, int prot) {
    if (!length || length > MAPPING_MAX || (sos2_ptr(address) & 4095)) {
        errno = EINVAL;
        return -1;
    }
    return sos2_result(call3(op, sos2_ptr(address), (length + 4095) / 4096, prot));
}

int munmap(void *address, size_t length) {
    if (sos2_load(&devices_open)) {
        int handled = 0;
        sos2_lock(&files_lock);
        for (unsigned i = 0; i < DEVICE_OPENS && !handled; ++i)
            if (device_opens[i].used && device_opens[i].device->munmap)
                handled = device_opens[i].device->munmap(device_opens[i].state, address, length);
        sos2_unlock(&files_lock);
        if (handled) return 0;
    }
    return memory_op(SYS_VM_UNMAP, address, length, 0);
}

int poll(struct pollfd *fds, nfds_t count, int timeout) {
    if (count > DESCRIPTORS * 4u) {
        errno = EINVAL;
        return -1;
    }
    int ready = 0;
    sos2_lock(&files_lock);
    for (nfds_t i = 0; i < count; ++i) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        if (fds[i].fd >= (int)DESCRIPTORS || !descriptors[fds[i].fd]) fds[i].revents = POLLNVAL;
        else fds[i].revents = fds[i].events & (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM);
        if (fds[i].revents) ++ready;
    }
    sos2_unlock(&files_lock);
    /* Nothing that can be waited for: sleep out the timeout. */
    if (!ready && timeout) {
        struct timespec delay = {timeout > 0 ? timeout / 1000 : 1,
                                 timeout > 0 ? (timeout % 1000) * 1000000L : 0};
        do nanosleep(&delay, NULL);
        while (timeout < 0);
    }
    return ready;
}

/* There is no system log: messages are discarded. setlogmask still reports
 * the previous mask, as programs may save and restore it. */
static int syslog_mask = 0xff;

void openlog(const char *ident, int option, int facility) {
    (void)ident;
    (void)option;
    (void)facility;
}

void closelog(void) { }

int setlogmask(int mask) {
    int old = syslog_mask;
    if (mask) syslog_mask = mask;
    return old;
}

void vsyslog(int priority, const char *format, va_list arguments) {
    (void)priority;
    (void)format;
    (void)arguments;
}

void syslog(int priority, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}

/* Programs are static: there is no dynamic loader. */
static char static_only[] = "SOS2 programs are statically linked";
static char *dl_message;

void *dlopen(const char *file, int mode) {
    (void)file;
    (void)mode;
    dl_message = static_only;
    return NULL;
}

void *dlsym(void *handle, const char *name) {
    (void)handle;
    (void)name;
    dl_message = static_only;
    return NULL;
}

int dlclose(void *handle) {
    (void)handle;
    return 0;
}

char *dlerror(void) {
    char *message = dl_message;
    dl_message = NULL;
    return message;
}

int dladdr(const void *address, Dl_info *info) {
    (void)address;
    (void)info;
    return 0;
}

int mprotect(void *address, size_t length, int prot) {
    /* As Linux: an empty range at an aligned address changes nothing. */
    if (!length && !(sos2_ptr(address) & 4095)) return 0;
    return memory_op(SYS_VM_PROTECT, address, length, prot);
}

int madvise(void *address, size_t length, int advice) {
    if (advice == MADV_DONTNEED) return memory_op(SYS_VM_DISCARD, address, length, 0);
    if (advice >= MADV_NORMAL && advice <= MADV_WILLNEED) return 0;
    errno = ENOTSUP;
    return -1;
}

int msync(void *address, size_t length, int flags) {
    if ((flags & ~(MS_ASYNC | MS_SYNC | MS_INVALIDATE)) ||
        ((flags & MS_ASYNC) && (flags & MS_SYNC))) {
        errno = EINVAL;
        return -1;
    }
    /* Only private mappings are supported. They have no writeback obligation. */
    return memory_op(SYS_VM_CHECK, address, length, 0);
}

int mincore(void *address, size_t length, unsigned char *resident) {
    if (!length || length > MAPPING_MAX || (sos2_ptr(address) & 4095)) {
        errno = EINVAL;
        return -1;
    }
    if (!resident) {
        errno = EFAULT;
        return -1;
    }
    unsigned pages = (length + 4095) / 4096;
    if (call3(SYS_VM_CHECK, sos2_ptr(address), pages, 0) < 0) {
        errno = ENOMEM;
        return -1;
    }
    /* Every SOS2 mapping currently has physical backing, including guards. */
    memset(resident, 1, pages);
    return 0;
}

#ifdef __x86_64__
/* x64's Newlib leaves the POSIX calls to the system, where ARM's wraps
 * them itself (its libc/syscalls): its reentrant layer (_read_r and the
 * others) calls these names. */
int close(int fd) { return _close(fd); }

int fstat(int fd, struct stat *out) { return _fstat(fd, out); }

pid_t getpid(void) { return _getpid(); }

int gettimeofday(struct timeval *restrict out, void *restrict zone) {
    return _gettimeofday(out, zone);
}

int isatty(int fd) { return _isatty(fd); }

int kill(pid_t pid, int signal) { return _kill(pid, signal); }

int link(const char *old, const char *new) { return _link(old, new); }

off_t lseek(int fd, off_t offset, int whence) { return _lseek(fd, offset, whence); }

int open(const char *path, int flags, ...) {
    va_list arguments;
    va_start(arguments, flags);
    int mode = flags & O_CREAT ? va_arg(arguments, int) : 0;
    va_end(arguments);
    return _open(path, flags, mode);
}

_READ_WRITE_RETURN_TYPE read(int fd, void *buffer, size_t length) {
    return _read(fd, buffer, length);
}

void *sbrk(ptrdiff_t change) { return _sbrk(change); }

int stat(const char *restrict path, struct stat *restrict out) { return _stat(path, out); }

clock_t times(struct tms *out) { return _times(out); }

int unlink(const char *path) { return _unlink(path); }

_READ_WRITE_RETURN_TYPE write(int fd, const void *buffer, size_t length) {
    return _write(fd, buffer, length);
}
#endif
