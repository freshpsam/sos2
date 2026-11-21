#ifndef SOS2_USER_ABI_H
#define SOS2_USER_ABI_H
/* The user address window. arm64's lies above 4 GiB, where truncating an
 * address to 32 bits faults instead of reaching other memory. */
#ifdef __aarch64__
#define USER_BASE        0x1000000000
#define USER_END         0x1100000000
#define WEB_IMAGE_BASE   0x1010000000
#define WEB_IMAGE_END    0x1020000000
/* The engine's JIT window: the only pages that may be writable and executable
 * at once, and only in web page processes. */
#define USER_JIT_BASE    0x1020000000
#define USER_JIT_END     0x1024000000
#define WEB_STACK_BOTTOM 0x10ffb80000
#define WEB_STACK_TOP    0x10fff80000
#define USER_HEAP_BASE   0x1040000000
#define USER_HEAP_END    0x10e0000000
#else
/* ARM32 maps the kernel and RAM where they lie, so the window ends where
 * QEMU's RAM starts (the Redmi 2's starts at 0x80000000). */
#define USER_BASE        0x10000000u
#define USER_END         0x40000000u
#define WEB_IMAGE_BASE   0x11000000u
#define WEB_IMAGE_END    0x18000000u
#define USER_JIT_BASE    0x18000000u
#define USER_JIT_END     0x1a000000u
#define WEB_STACK_BOTTOM 0x3fd80000u
#define WEB_STACK_TOP    0x3ff80000u
#define USER_HEAP_BASE   0x1a000000u
#define USER_HEAP_END    0x3f000000u
#endif
/* A web page process starts with x2 (r2 on ARM32) pointing to the sos2.jsc=
 * boot argument's value above its initial stack (NUL-terminated, at most this
 * many bytes with the NUL), or 0 without one. */
#define USER_ENGINE_OPTIONS_MAX 128u
/* Each process's memory: 544 MiB on ARM32, its whole mapping window, or 1 GiB
 * for the 64-bit engine, whose pages take more (wider objects, a committed JIT
 * pool and structure heap). Heavy sites need most of it: Reddit's page passes
 * 135 MiB on ARM32 and 340 MiB on AArch64 within two minutes, and YouTube
 * playing a video holds 475 MiB on ARM32 (481 MiB at its peak) and 581 MiB on
 * AArch64. */
#ifdef __aarch64__
#define USER_PAGE_QUOTA      262144u
#else
#define USER_PAGE_QUOTA      139264u
#endif
#define SYS_EXIT             0
#define SYS_YIELD            1
#define SYS_SLEEP            2
#define SYS_GETPID           4
#define SYS_PRESENT          8
#define SYS_NEXT_EVENT       52
#define SYS_INPUT_METHOD     53
#define SYS_NET_INFO         54
#define SYS_NET_CONNECT      55
#define SYS_NET_STATUS       56
#define SYS_NET_SEND         57
#define SYS_NET_RECV         58
#define SYS_NET_CLOSE        59
#define SYS_FILE_OPEN        11
#define SYS_FILE_READ        12
#define SYS_FILE_SEEK        13
#define SYS_FILE_SIZE        14
#define SYS_FILE_CLOSE       15
#define SYS_FILE_WRITE       16
#define SYS_FILE_TRUNCATE    17
#define SYS_FILE_SYNC        18
#define SYS_PATH_MKDIR       19
#define SYS_PATH_REMOVE      20
#define SYS_PATH_RENAME      21
#define SYS_FILE_INFO        22
#define SYS_FILE_NEXT        23
#define SYS_PATH_INFO        24
#define SYS_FS_INFO          25
#define SYS_THREAD_CREATE    32
#define SYS_THREAD_EXIT      33
#define SYS_THREAD_JOIN      34
#define SYS_THREAD_ID        35
#define SYS_THREAD_TLS       36
#define SYS_WAIT_WORD        37
#define SYS_WAKE_WORD        38
#define SYS_CLOCK            39
#define SYS_RANDOM           40
#define SYS_VM_MAP           41
#define SYS_VM_PROTECT       42
#define SYS_VM_UNMAP         43
#define SYS_VM_DISCARD       44
#define SYS_THREAD_INFO      45
#define SYS_THREAD_SUSPEND   46
#define SYS_THREAD_RESUME    47
#define SYS_THREAD_CONTEXT   48
#define SYS_MEMORY_INFO      49
#define SYS_VM_CHECK         50
#define SYS_VM_ALLOC_RANGE   51
#define SYS_BROWSER_COMMAND  60
#define SYS_BROWSER_STATE    61
#define SYS_PRESENT_DAMAGE   62
#define SYS_SURFACE_STATE    63
#define SYS_CPU_COUNT        64
#define SYS_PRESENT_FRAME    65
#define SYS_GPU              67
#define SYS_BACK_KEY         68
#define SYS_FILE_LOCK        69
#define SYS_HAPTIC_TAP       70
#define SYS_UPDATE           71
/* ARM32 only: makes code just written into the JIT window (address, size)
 * visible to instruction fetch, which AArch32 user mode cannot do itself.
 * Handled on the calling CPU without ending its slice; 0 or USER_EINVAL. */
#define SYS_ICACHE_SYNC      72
/* Audio output: sos2_audio (sdk/include/sos2/services.h). */
#define SYS_AUDIO            73
#define SYS_COUNT            74
#define USER_FRAME_RECTS     12u
#define USER_SURFACE_VISIBLE 1u
/* The display shows the page's GPU frames (USER_GPU_PRESENT). Once clear, a
 * failed GPU, it stays clear: the page must present CPU frames. */
#define USER_SURFACE_GPU_FRAMES 2u
/* Threads per process: a page's GC markers, compiler threads, image decoders
 * and its sites' workers. */
#define USER_THREAD_LIMIT       64u
/* A thread's stack pointer and stack size alignment. */
#ifdef __aarch64__
#define USER_STACK_ALIGN 16u
#else
#define USER_STACK_ALIGN 8u
#endif
#define USER_WAIT_FOREVER       0xffffffffu
#define USER_PROT_READ          1u
#define USER_PROT_WRITE         2u
#define USER_PROT_EXEC          4u
/* SYS_VM_ALLOC_RANGE's flags: USER_PROT_* and, from this bit, log2 of the
 * mapping's alignment in pages, up to USER_MAP_ALIGN_MAX (1 GiB). */
#define USER_MAP_ALIGN_SHIFT    8u
#define USER_MAP_ALIGN_MAX      18u
#define USER_FILE_HANDLES       64u
#define USER_PATH_MAX           127u
#define USER_FILE_READ_MAX      65536u
#define FILE_READ               1u
#define FILE_WRITE              2u
#define FILE_CREATE             4u
#define FILE_EXCLUSIVE          8u
#define FILE_TRUNCATE           16u
#define FILE_APPEND             32u
#define FILE_DIRECTORY          64u
#define FILE_REGULAR            1u
#define FILE_DIR                2u
#ifndef __ASSEMBLER__
#include <stdint.h>

/* A user address, and the ABI's pointer-sized fields: 32 bits on ARM32 and
 * 64 bits on arm64. Structures the kernel fills hold only pointer-sized
 * fields, so they have no padding on arm64 either. */
#ifdef __aarch64__
typedef uint64_t user_ptr;
#else
typedef uint32_t user_ptr;
#endif

/* Microseconds from a generic timer count. The kernel's clock and wait
 * deadlines and the SDK's CLOCK_MONOTONIC both convert with it, so they agree. */
static inline uint64_t user_timer_us(uint64_t ticks, uint32_t frequency) {
    return (ticks / frequency) * 1000000 + (ticks % frequency) * 1000000 / frequency;
}

struct user_file_info {
    uint32_t type, size;
};

struct user_fs_info {
    uint32_t block_size, blocks, free_blocks, name_max, read_only;
};

struct user_memory_info {
    uint32_t page_size, current_pages, peak_pages, quota_pages, available_pages;
};

/* SYS_AUDIO: one output stream per process, of interleaved signed 16-bit
 * stereo frames at USER_AUDIO_RATE; the kernel mixes the open streams. OPEN
 * starts the stream empty; WRITE queues up to count frames from argument and
 * returns how many it took, never blocking (0 while the queue is full); FLUSH
 * drops the queued frames; PAUSE holds the stream (argument 1) or resumes it
 * (0); STATUS fills a struct user_audio_status at argument. Without an audio
 * device a stream still plays, silently, at the same rate. */
#define USER_AUDIO_RATE     48000u
#define USER_AUDIO_CHANNELS 2u
#define USER_AUDIO_OPEN     0u
#define USER_AUDIO_WRITE    1u
#define USER_AUDIO_FLUSH    2u
#define USER_AUDIO_PAUSE    3u
#define USER_AUDIO_STATUS   4u
#define USER_AUDIO_CLOSE    5u
#define USER_AUDIO_DEVICE   1u /* user_audio_status.flags: a device plays the streams */
struct user_audio_status {
    uint64_t played;   /* frames of this stream played since OPEN */
    uint32_t queued;   /* frames waiting to play */
    uint32_t capacity; /* the queue's size in frames */
    uint32_t latency;  /* frames between leaving the queue and being heard */
    uint32_t flags;
};

struct user_dir_entry {
    uint32_t type, size;
    char name[128];
};

struct user_thread_start {
    user_ptr entry, argument, stack, size, tls, completion;
};

struct user_thread_info {
    user_ptr id, stack, size, tls;
};

/* The registers: on arm64 x0-x30, sp, pc and pstate, then v0-v31 (two words
 * each), FPSR and FPCR; on ARM32 r0-r12, sp, lr, pc and cpsr. */
#ifdef __aarch64__
struct user_thread_context {
    uint64_t r[31], sp, pc, pstate;
    uint64_t v[64];
    uint32_t fpsr, fpcr;
};
#else
struct user_thread_context {
    uint32_t r[13], sp, lr, pc, cpsr;
};
#endif

/* Damage is in the fixed WEB_WIDTH by WEB_HEIGHT pixel buffer. */
struct user_present_damage {
    user_ptr pixels;
    uint32_t x, y, width, height;
};

struct user_pixel_rect {
    uint32_t x, y, width, height;
};

/* SYS_GPU (QEMU virgl only): each page gets its own virgl context, created on
 * first use. Resources are attached to that context alone, so command streams
 * cannot reach other pages' or the compositor's resources. Handles are device
 * resource ids, as used inside command streams. Operations run in order:
 * success means the device processed the request, so later operations see
 * its effects, and downloads return finished rendering. Uploads and
 * downloads copy rows between page memory and the resource's backing, using
 * the same offset, stride and layer stride on both sides; zero strides mean
 * the level's packed rows and the box's rows, as in the virgl protocol. */
#define USER_GPU_INFO      0
#define USER_GPU_CAPS      1
#define USER_GPU_CREATE    2
#define USER_GPU_DESTROY   3
#define USER_GPU_UPLOAD    4
#define USER_GPU_DOWNLOAD  5
#define USER_GPU_SUBMIT    6
#define USER_GPU_PRESENT   7
#define USER_GPU_RESOURCES 1024u
/* Mesa's largest virgl command buffer: 65536 + 1024 words. */
#define USER_GPU_STREAM_MAX   (260u * 1024u)
#define USER_GPU_RESOURCE_MAX (16u * 1024u * 1024u)
#define USER_GPU_PAGE_MAX     (64u * 1024u * 1024u)
/* The virgl formats resources may use, as X(format, bytes per pixel);
 * buffers are R8 (64). */
#define USER_GPU_FORMATS(X) \
    X(1,4) X(2,4) X(67,4) X(134,4)         /* BGRA/BGRX, RGBA/RGBX */ \
    X(19,4) X(20,4)                        /* Z24S8, S8Z24: hosts offer one */ \
    X(7,2) X(16,2) X(48,2) X(65,2)         /* B5G6R5, Z16, R16, RG8 */ \
    X(10,1) X(23,1) X(64,1)                /* A8, S8, R8 */ \
    X(94,8) X(31,16)                       /* RGBA16F, RGBA32F */

struct user_gpu_caps {
    uint32_t capset;
    user_ptr buffer;
    uint32_t size;
};

struct user_gpu_resource {
    uint32_t handle, target, format, bind, width, height, depth, array_size, last_level, nr_samples,
        flags, size;
};

struct user_gpu_transfer {
    uint32_t handle, level, x, y, z, width, height, depth, offset, stride, layer_stride;
    user_ptr data;
};

struct user_gpu_submit {
    user_ptr commands;
    uint32_t bytes;
};

/* USER_GPU_PRESENT shows one of the page's resources as the page: a 2D
 * WEB_WIDTH x WEB_HEIGHT B8G8R8A8 or B8G8R8X8 resource bound for rendering and
 * sampling, top row first. Before returning, the kernel copies the damaged
 * rectangles into the compositor's snapshot of the page, so the page may draw
 * its next frame at once. A page's first GPU frame, and its first CPU frame
 * after GPU frames, must cover the whole page. */
struct user_gpu_present {
    uint32_t handle, count;
    struct user_pixel_rect damage[USER_FRAME_RECTS];
};

/* One retained scroll, then patches from the fixed-stride pixel buffer. A zero
 * scroll rectangle disables scrolling. No handles or GPU commands cross here.
 * The kernel validates the entire frame before changing the retained surface. */
struct user_pixel_scroll {
    struct user_pixel_rect rect;
    int32_t dx, dy;
};

struct user_present_frame {
    user_ptr pixels;
    uint32_t count;
    struct user_pixel_scroll scroll;
    struct user_pixel_rect damage[USER_FRAME_RECTS];
};

/* SYS_UPDATE(operation, address, size): system and app updates, for the
 * system's updater (/bin/sos2-updater, programs/updater) alone; other
 * processes get USER_ENOSYS. BEGIN takes the USER_UPDATE_HEADER bytes that
 * start a file, a device's system update (sos2-redmi2.update and the others)
 * or an app's .sos2app, WRITE the bytes after them in order, at most
 * USER_UPDATE_WRITE_MAX at a time, and FINISH, called until it stops
 * returning USER_EAGAIN, checks the file and installs it. Only a device
 * started from its installed system installs, and INFO says so with
 * USER_UPDATE_INSTALLS: elsewhere BEGIN and REMOVE return USER_ENODEV. APPS
 * fills an array of struct user_update_app and returns how many it filled;
 * REMOVE removes the installed app whose id address holds (size 16). CANCEL
 * drops the caller's unfinished file, as when its download failed, and
 * leaves USER_UPDATE_IDLE. Once a system update writes the boot image, it
 * finishes even if its updater ends, and BEGIN, CANCEL and RESTART return
 * USER_EBUSY until it has. RESTART restarts the device; DOWNLOAD restarts it
 * into its bootloader's download mode, where INFO's USER_UPDATE_DOWNLOAD_MODE
 * says it can (the Galaxy Tab A7 Lite's), and returns USER_ENODEV elsewhere.
 * INFO fills struct user_update_info, whose board names the device as its
 * system updates do (include/system_store.h). Address and size are 0 where
 * unused. A failure leaves USER_UPDATE_FAILED and a message. */
#define USER_UPDATE_INFO      0
#define USER_UPDATE_BEGIN     1
#define USER_UPDATE_WRITE     2
#define USER_UPDATE_FINISH    3
#define USER_UPDATE_RESTART   4
#define USER_UPDATE_APPS      5
#define USER_UPDATE_REMOVE    6
#define USER_UPDATE_CANCEL    7
#define USER_UPDATE_DOWNLOAD  8
#define USER_UPDATE_HEADER    4096u
#define USER_UPDATE_WRITE_MAX 65536u
/* States: RECEIVING after BEGIN; WRITING while FINISH writes a system
 * update's boot image, then CHECKING while it reads it back; DONE once a file
 * is installed or an app removed. */
#define USER_UPDATE_IDLE      0
#define USER_UPDATE_RECEIVING 1
#define USER_UPDATE_CHECKING  2
#define USER_UPDATE_WRITING   3
#define USER_UPDATE_DONE      4
#define USER_UPDATE_FAILED    5
/* flags: this boot installs files, else BEGIN refuses them; an installed
 * system update waits for a restart; DOWNLOAD works here. */
#define USER_UPDATE_INSTALLS      1u
#define USER_UPDATE_RESTART_DUE   2u
#define USER_UPDATE_DOWNLOAD_MODE 4u
/* An app's flags: installed from a package, else built in; running; built
 * for another USER_ABI_VERSION, so it does not start until it is updated. */
#define USER_APP_STORED  1u
#define USER_APP_RUNNING 2u
#define USER_APP_STALE   4u
/* The ABI native apps are built for (include/app_store.h): the kernel starts
 * only apps built for its own. Raise it when a change breaks programs built
 * before it. arm64's carries EM_AARCH64 (183) above it, so the two never
 * match. tools/package_redmi2.py reads both from these defines. */
#ifndef __aarch64__
#define USER_ABI_VERSION 1
#else
#define USER_ABI_VERSION ((183u << 16) | 1)
#endif

struct user_update_info {
    uint32_t state, flags, done, total; /* done/total: bytes of the state's work */
    char release[48];                   /* the running release, or empty */
    char message[64];                   /* what failed, or the state's detail */
    char board[16];                     /* the device's, as its system updates name it */
};

struct user_update_app {
    char id[16], name[16];
    uint8_t sha256[32]; /* an installed app's package; zero when built in */
    uint32_t flags, abi;
};
#endif
#define WEB_WIDTH         480u
#define WEB_HEIGHT        752u
#define WEB_TOP           48u
#define USER_ENOENT       (-2)
#define USER_ESRCH        (-3)
#define USER_EIO          (-5)
#define USER_EBADF        (-9)
#define USER_EAGAIN       (-11)
#define USER_ENOMEM       (-12)
#define USER_EFAULT       (-14)
#define USER_EBUSY        (-16)
#define USER_EEXIST       (-17)
#define USER_EXDEV        (-18)
#define USER_ENODEV       (-19)
#define USER_ENETDOWN     (-100)
#define USER_ENETUNREACH  (-101)
#define USER_ECONNABORTED (-103)
#define USER_ECONNRESET   (-104)
#define USER_ECONNREFUSED (-111)
#define USER_EHOSTUNREACH (-113)
#define USER_ENOTDIR      (-20)
#define USER_EISDIR       (-21)
#define USER_EINVAL       (-22)
#define USER_EMFILE       (-24)
#define USER_EFBIG        (-27)
#define USER_ENOSPC       (-28)
#define USER_EROFS        (-30)
#define USER_EDEADLK      (-35)
#define USER_ENAMETOOLONG (-36)
#define USER_ENOSYS       (-38)
#define USER_ENOTEMPTY    (-39)
#define USER_ETIMEDOUT    (-110)
#endif
