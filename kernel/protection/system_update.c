#include "system_update.h"
#include "system_store.h"
#include "app_store.h"
#include "page_host.h"
#include "process.h"
#include "boot_bundle.h"
#include "boot_memory.h"
#include "boot_progress.h"
#include "elf_load.h"
#include "fs.h"
#include "runtime_files.h"
#include "kernel_io.h"
#include "sha256.h"
#include "platform.h"
#include <stddef.h>
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_power.h"
#else
extern int32_t arm_psci_cpu_on(unsigned method, uint32_t function, uint32_t a, uint32_t b);
#endif

/* The formats are in include/system_store.h, written by
 * tools/package_redmi2.py. An update is installed in the order that keeps the
 * phone starting at every step: the cache slot the boot image does not use
 * loses its header, takes the resources and is read back against their
 * SHA-256, gets its header again; then the boot image loses its first sector,
 * which lk2nd needs to start it, takes the rest, and gets that sector last.
 * Until then the old boot image and its slot are untouched, and an
 * interrupted boot-image write leaves lk2nd's fastboot. An app's package
 * (include/app_store.h) arrives whole in RAM, is checked, and goes to its
 * app store position the same way. Nothing is signed: the SHA-256s guard
 * against damage, not against whoever publishes the files. */
enum {
    ANDROID_PAGE = 2048,
    KERNEL_ADDRESS = 0x80008000,
    TAGS_ADDRESS = 0x81e00000,
    RESOURCES_MAX = BOOT_RAMDISK_END - BOOT_RAMDISK_BASE,
    WORK_US = 2000,   /* of eMMC or hashing work before the desktop runs */
    CHECK_US = 250000 /* of reading back per FINISH, between app updates */
};

_Static_assert(SYSTEM_SLOT_HEADER + RESOURCES_MAX <= SYSTEM_SLOT_SECTORS * 512,
               "a slot holds the largest resources the kernel reads");

static const char board[16] = "redmi2-wt88047";

static struct {
    uint32_t state, owner; /* owner: the receiving process */
    bool installs;
    struct system_update_header header;
    struct sha256 boot_hash, system_hash;
    unsigned char *buffer; /* the boot image, or an app's whole package */
    unsigned pages, slot;
    bool app;                          /* receiving an app's package */
    uint32_t received, total, checked; /* bytes of the file; of the slot read back */
    uint64_t written;                  /* whole sectors of the slot's data */
    unsigned char tail[512];           /* data short of a whole sector */
    unsigned tail_used;
    char message[64];
} update;

static char release[SYSTEM_RELEASE_MAX + 1];
static bool restart_due; /* an installed system update waits for a restart */
/* The slot the boot partition's image starts from: the one this boot read,
 * until an install names the other. */
static unsigned boot_slot;
#ifdef SOS2_BOARD_REDMI2
static bool installed; /* started from the installed system */
#else
static enum cpu_boot_method psci_method; /* QEMU restarts through PSCI */
#endif
static uint64_t worked_at;
/* The service worker is serialized, so one buffer serves every request. */
static uint32_t scratch[1024], sector[128];

static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool printable(const char *text, size_t length) {
    for (size_t i = 0; i < length; ++i)
        if (text[i] < ' ' || text[i] > '~') return false;
    return true;
}

/* Let the desktop run between pieces of work on the service worker; a no-op
 * during boot. */
static void work(void) {
    if (time_us() - worked_at < WORK_US) return;
    kernel_io_wait();
    worked_at = time_us();
}

/* The next token of a command line at most length bytes long, or 0. */
static const char *token(const char *args, size_t length, size_t *at, size_t *n) {
    while (*at < length && (args[*at] == ' ' || args[*at] == '\t')) ++*at;
    size_t start = *at;
    while (*at < length && args[*at] && args[*at] != ' ' && args[*at] != '\t') ++*at;
    *n = *at - start;
    return *n ? args + start : 0;
}

static int nibble(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* sos2.system=<64 lowercase hex digits>: 1 with its SHA-256, 0 when absent,
 * -1 when malformed or repeated. */
static int system_argument(const char *args, size_t length, uint8_t out[32]) {
    static const char prefix[] = "sos2.system=";
    const size_t skip = sizeof(prefix) - 1;
    int found = 0;
    size_t at = 0, n;
    for (const char *t; (t = token(args, length, &at, &n));) {
        if (n < skip || memcmp(t, prefix, skip)) continue;
        if (found || n != skip + 64) return -1;
        for (unsigned i = 0; i < 32; ++i) {
            int high = nibble(t[skip + 2 * i]), low = nibble(t[skip + 2 * i + 1]);
            if (high < 0 || low < 0) return -1;
            out[i] = (uint8_t)(high << 4 | low);
        }
        found = 1;
    }
    return found;
}

/* The storage mode as fs.c reads it at boot: one sos2.storage= token, here
 * sos2.storage=installed. */
static bool storage_installed(const char *args, size_t length) {
    static const char prefix[] = "sos2.storage=";
    const size_t skip = sizeof(prefix) - 1;
    bool found = false;
    size_t at = 0, n;
    for (const char *t; (t = token(args, length, &at, &n));) {
        if (n < skip || memcmp(t, prefix, skip)) continue;
        if (found || n != skip + 9 || memcmp(t + skip, "installed", 9)) return false;
        found = true;
    }
    return found;
}

static uint64_t slot_sector(unsigned slot) {
    return SYSTEM_SLOT_START + (uint64_t)slot * SYSTEM_SLOT_SECTORS;
}

static bool slot_read(unsigned slot, struct system_slot *out) {
    if (!system_store_read(SYSTEM_CACHE, slot_sector(slot), sector, 1)) return false;
    memcpy(out, sector, sizeof(*out));
    return !memcmp(out->magic, "SOS2SLOT", 8) && out->version == 1 &&
           out->data_offset == SYSTEM_SLOT_HEADER && out->data_size >= 32 &&
           out->data_size <= RESOURCES_MAX && !out->reserved &&
           crc32_ieee(out, offsetof(struct system_slot, crc32)) == out->crc32;
}

#ifdef SOS2_BOARD_REDMI2
static uint8_t running[32];

bool system_boot_load(struct platform_devices *d, uintptr_t dtb, uintptr_t ks, uintptr_t ke) {
    int named = system_argument(d->bootargs, sizeof(d->bootargs), running);
    if (named < 0) panic("invalid sos2.system boot argument");
    if (!named) return false;
    installed = true;
    boot_progress_report(BOOT_READ_ENGINE, 0, 0);
    if (!system_store_init(d))
        panic("Installed SOS2: the eMMC's boot and cache partitions are unavailable");
    bool found = false;
    for (unsigned slot = 0; slot < 2; ++slot) {
        struct system_slot s;
        if (!slot_read(slot, &s) || memcmp(s.sha256, running, 32)) continue;
        found = true;
        uint32_t size = s.data_size, sectors = (size + 511) / 512;
        /* The page-rounded range covers the last sector's padding too. */
        if (!boot_memory_available(d, BOOT_RAMDISK_BASE, size, dtb, ks, ke, false))
            panic("Installed SOS2: no free RAM to read its resources into");
        unsigned char *to = (unsigned char *)BOOT_RAMDISK_BASE;
        uint64_t first = slot_sector(slot) + SYSTEM_SLOT_HEADER / 512;
        bool read = true;
        for (uint32_t done = 0; read && done < sectors;) {
            unsigned n = sectors - done < 128 ? sectors - done : 128;
            boot_progress_report(BOOT_READ_ENGINE, done * 512, size);
            read = system_store_read(SYSTEM_CACHE, first + done, to + (size_t)done * 512, n);
            done += n;
        }
        if (!read) break;
        boot_progress_report(BOOT_READ_ENGINE, size, size);
        d->has_initrd_start = d->has_initrd_end = true;
        d->initrd_start = BOOT_RAMDISK_BASE;
        d->initrd_end = BOOT_RAMDISK_BASE + size;
        if (boot_bundle_prepare(d, dtb, ks, ke)) {
            boot_slot = slot;
            return true;
        }
    }
    if (!system_store_sectors(SYSTEM_CACHE))
        panic("Installed SOS2: the eMMC failed while reading its resources");
    panic(found ? "Installed SOS2: its resources fail their check; flash flash-cache.img again"
                : "Installed SOS2: no cache slot holds its resources; flash flash-cache.img");
}
#endif

void system_update_init(const struct platform_devices *d) {
#ifndef SOS2_BOARD_REDMI2
    psci_method = d->psci_method;
#else
    (void)d;
#endif
    release[0] = 0;
    const struct runtime_file *f = runtime_file_find("/system/release");
    if (f && f->size && f->size <= SYSTEM_RELEASE_MAX && printable(runtime_file_data(f), f->size)) {
        memcpy(release, runtime_file_data(f), f->size);
        release[f->size] = 0;
    }
    update = (__typeof__(update)){0};
#ifdef SOS2_BOARD_REDMI2
    /* Read from the eMMC, it installs there; BEGIN tells a failed card. */
    update.installs = installed;
#endif
}

static void say(const char *text) {
    size_t n = strlen(text);
    if (n >= sizeof(update.message)) n = sizeof(update.message) - 1;
    memcpy(update.message, text, n);
    update.message[n] = 0;
}

/* Release what receiving held; the state and message stay. */
static void drop(void) {
    if (update.buffer) vm_cpu_free(update.buffer, update.pages);
    update.buffer = 0;
    update.pages = 0;
    update.owner = 0;
}

static int32_t failure(const char *text, int32_t error) {
    drop();
    update.state = USER_UPDATE_FAILED;
    say(text);
    return error;
}

static const char storage_failed[] = "Storage failed; restart to try again";

int32_t system_update_info(struct vm_space *space, uint32_t address, uint32_t size) {
    struct user_update_info info = {.state = update.state,
                                    .flags = (update.installs ? USER_UPDATE_INSTALLS : 0) |
                                             (restart_due ? USER_UPDATE_RESTART_DUE : 0)};
    if (update.state == USER_UPDATE_RECEIVING) {
        info.done = update.received;
        info.total = update.total;
    } else if (update.state == USER_UPDATE_CHECKING) {
        info.done = update.checked;
        info.total = update.header.system_size;
    }
    memcpy(info.release, release, sizeof(release));
    memcpy(info.message, update.message, sizeof(update.message));
    if (size != sizeof(info)) return USER_EINVAL;
    return vm_copy_to_user(space, address, &info, sizeof(info)) ? 0 : USER_EFAULT;
}

static bool header_valid(const struct system_update_header *h) {
    const char *end = memchr(h->release, 0, sizeof(h->release));
    size_t length = end ? (size_t)(end - h->release) : 0;
    for (size_t i = length; end && i < sizeof(h->release); ++i)
        if (h->release[i]) return false;
    uint64_t system_offset = h->boot_offset + (((uint64_t)h->boot_size + 4095) & ~4095ull);
    return crc32_ieee(h, offsetof(struct system_update_header, crc32)) == h->crc32 &&
           !memcmp(h->magic, "SOS2UPDT", 8) && h->version == 1 &&
           h->header_size == SYSTEM_UPDATE_HEADER && !memcmp(h->board, board, sizeof(board)) &&
           length && printable(h->release, length) && h->boot_offset == SYSTEM_UPDATE_HEADER &&
           h->boot_size > ANDROID_PAGE && h->boot_size <= SYSTEM_BOOT_LIMIT &&
           !(h->boot_size % ANDROID_PAGE) && h->system_offset == system_offset &&
           h->system_size >= 32 && h->system_size <= RESOURCES_MAX &&
           h->total_size == system_offset + h->system_size;
}

/* An app's package: its header checked now, the rest kept in RAM. */
static int32_t app_begin(uint32_t pid, const unsigned char *bytes) {
    struct app_package package;
    memcpy(&package, bytes, sizeof(package));
    if (!app_package_valid(&package, package.size)) return failure("Not an SOS2 app", USER_EINVAL);
    if (package.abi != USER_ABI_VERSION)
        return failure("Needs another system release", USER_EINVAL);
    update.pages = (package.size + PAGE_SIZE - 1) / PAGE_SIZE;
    update.buffer = vm_cpu_alloc(update.pages);
    if (!update.buffer) return failure("Not enough memory for the app", USER_ENOMEM);
    memcpy(update.buffer, bytes, SYSTEM_UPDATE_HEADER);
    update.app = true;
    update.total = package.size;
    update.received = SYSTEM_UPDATE_HEADER;
    update.owner = pid;
    update.state = USER_UPDATE_RECEIVING;
    return 0;
}

static int32_t begin(uint32_t pid, struct vm_space *space, uint32_t address, uint32_t size) {
    /* Only the installed system takes files, to install them. */
    if (!update.installs) return USER_ENODEV;
    if ((update.state == USER_UPDATE_RECEIVING || update.state == USER_UPDATE_CHECKING ||
         update.state == USER_UPDATE_WRITING) &&
        update.owner != pid)
        return USER_EBUSY;
    drop();
    update.state = USER_UPDATE_IDLE;
    update.message[0] = 0;
    if (size != SYSTEM_UPDATE_HEADER || sizeof(scratch) != SYSTEM_UPDATE_HEADER) return USER_EINVAL;
    if (!vm_copy_from_user(space, scratch, address, size)) return USER_EFAULT;
    /* An eMMC error stops the card for the rest of the boot. */
    if (!system_store_sectors(SYSTEM_CACHE)) return failure(storage_failed, USER_EIO);
    const unsigned char *bytes = (const unsigned char *)scratch;
    if (!memcmp(bytes, "SOS2APP1", 8)) return app_begin(pid, bytes);
    memcpy(&update.header, bytes, sizeof(update.header));
    for (size_t i = sizeof(update.header); i < size; ++i)
        if (bytes[i]) return failure("Not an SOS2 update", USER_EINVAL);
    if (!header_valid(&update.header))
        return failure("Not an SOS2 update for this phone", USER_EINVAL);
    if (update.header.boot_size > system_store_sectors(SYSTEM_BOOT) * 512)
        return failure("Boot image does not fit", USER_EFBIG);
    /* The slot the boot image does not start from; once an install has named
     * the other, even the one this boot read, which RAM holds. */
    update.slot = 1 - boot_slot;
    /* A slot is whole once its header is back, written last. */
    memset(sector, 0, sizeof(sector));
    if (!system_store_write(SYSTEM_CACHE, slot_sector(update.slot), sector, 1) ||
        !system_store_sync())
        return failure(storage_failed, USER_EIO);
    update.pages = (update.header.boot_size + PAGE_SIZE - 1) / PAGE_SIZE;
    update.buffer = vm_cpu_alloc(update.pages);
    if (!update.buffer) return failure("Not enough memory for the update", USER_ENOMEM);
    sha256_begin(&update.boot_hash);
    sha256_begin(&update.system_hash);
    update.app = false;
    update.total = update.header.total_size;
    update.received = SYSTEM_UPDATE_HEADER;
    update.checked = 0;
    update.written = 0;
    update.tail_used = 0;
    update.owner = pid;
    update.state = USER_UPDATE_RECEIVING;
    return 0;
}

static bool write_sectors(const void *data, unsigned count) {
    uint64_t at = slot_sector(update.slot) + SYSTEM_SLOT_HEADER / 512 + update.written;
    if (!system_store_write(SYSTEM_CACHE, at, data, count)) return false;
    update.written += count;
    work();
    return true;
}

/* The resources into the slot, a whole sector at a time. */
static bool store(const unsigned char *p, uint32_t n) {
    while (n) {
        if (update.tail_used || n < 512) {
            uint32_t take = 512 - update.tail_used < n ? 512 - update.tail_used : n;
            memcpy(update.tail + update.tail_used, p, take);
            update.tail_used += take;
            p += take;
            n -= take;
            if (update.tail_used == 512) {
                if (!write_sectors(update.tail, 1)) return false;
                update.tail_used = 0;
            }
        } else {
            unsigned count = n / 512 < 8 ? n / 512 : 8;
            if (!write_sectors(p, count)) return false;
            p += count * 512;
            n -= count * 512;
        }
    }
    return true;
}

/* The file's bytes in order: the boot image, zero padding, the resources. */
static const char *consume(const unsigned char *p, uint32_t n) {
    const struct system_update_header *h = &update.header;
    while (n) {
        uint32_t at = update.received, take;
        if (at < h->boot_offset + h->boot_size) {
            take = h->boot_offset + h->boot_size - at < n ? h->boot_offset + h->boot_size - at : n;
            memcpy(update.buffer + (at - h->boot_offset), p, take);
            sha256_add(&update.boot_hash, p, take);
        } else if (at < h->system_offset) {
            take = h->system_offset - at < n ? h->system_offset - at : n;
            for (uint32_t i = 0; i < take; ++i)
                if (p[i]) return "The download is damaged";
        } else {
            take = n;
            sha256_add(&update.system_hash, p, take);
            if (!store(p, take)) return storage_failed;
        }
        update.received += take;
        p += take;
        n -= take;
    }
    return 0;
}

static int32_t receive(uint32_t pid, struct vm_space *space, uint32_t address, uint32_t size) {
    if (update.state != USER_UPDATE_RECEIVING || update.owner != pid || !size ||
        size > USER_UPDATE_WRITE_MAX)
        return USER_EINVAL;
    if (size > update.total - update.received)
        return failure("Download longer than announced", USER_EIO);
    for (uint32_t done = 0; done < size;) {
        uint32_t n = size - done < sizeof(scratch) ? size - done : sizeof(scratch);
        if (!vm_copy_from_user(space, scratch, address + done, n))
            return failure("Updater passed a bad buffer", USER_EFAULT);
        if (update.app) {
            memcpy(update.buffer + update.received, scratch, n);
            update.received += n;
        } else {
            const char *problem = consume((const unsigned char *)scratch, n);
            if (problem) return failure(problem, USER_EIO);
        }
        done += n;
        work();
    }
    return 0;
}

/* An image lk2nd starts as the installed system: the kernel at its address
 * with the DTB after it, no ramdisk or second stage, and a command line that
 * keeps data in RAM and names the update's resources, parsed as the next boot
 * will: lk2nd's bootargs start with it, the kernel keeps their first 255
 * bytes, and nothing else may join it. */
static bool boot_image_valid(void) {
    const unsigned char *image = update.buffer;
    const struct system_update_header *h = &update.header;
    uint32_t kernel = u32(image + 8);
    if (memcmp(image, "ANDROID!", 8) || kernel < 64 || u32(image + 12) != KERNEL_ADDRESS ||
        u32(image + 16) || u32(image + 24) || u32(image + 32) != TAGS_ADDRESS ||
        u32(image + 36) != ANDROID_PAGE ||
        ANDROID_PAGE + ((uint64_t)kernel + ANDROID_PAGE - 1) / ANDROID_PAGE * ANDROID_PAGE !=
            h->boot_size ||
        u32(image + ANDROID_PAGE + 0x24) != 0x016f2818)
        return false;
    const char *cmdline = (const char *)image + 64, *end = memchr(cmdline, 0, 512);
    for (unsigned i = 608; i < 608 + 1024; ++i) /* the extra command line */
        if (image[i]) return false;
    uint8_t named[32];
    return end && (size_t)(end - cmdline) < sizeof(((struct platform_devices *)0)->bootargs) &&
           system_argument(cmdline, 512, named) == 1 && !memcmp(named, h->system_sha256, 32) &&
           storage_installed(cmdline, 512);
}

static bool write_boot(uint32_t first, uint32_t count) {
    for (uint32_t at = first; at < first + count;) {
        unsigned n = first + count - at < 8 ? first + count - at : 8;
        if (!system_store_write(SYSTEM_BOOT, at, update.buffer + (size_t)at * 512, n)) return false;
        at += n;
        work();
    }
    return true;
}

static bool boot_matches(uint32_t first, uint32_t count) {
    for (uint32_t at = first; at < first + count;) {
        unsigned n = first + count - at < 8 ? first + count - at : 8;
        if (!system_store_read(SYSTEM_BOOT, at, scratch, n) ||
            memcmp(scratch, update.buffer + (size_t)at * 512, n * 512))
            return false;
        at += n;
        work();
    }
    return true;
}

/* The package against its SHA-256, a program the kernel can load, then the
 * app store. */
static int32_t app_finish(void) {
    if (update.state != USER_UPDATE_RECEIVING) return USER_EINVAL;
    if (update.received != update.total) return failure("The download ended early", USER_EIO);
    struct app_package package;
    memcpy(&package, update.buffer, sizeof(package));
    struct sha256 hash;
    uint8_t digest[32];
    sha256_begin(&hash);
    for (uint32_t at = APP_PACKAGE_HEADER; at < update.total; at += 65536) {
        sha256_add(&hash, update.buffer + at,
                   update.total - at < 65536 ? update.total - at : 65536);
        work();
    }
    sha256_end(&hash, digest);
    if (memcmp(digest, package.sha256, 32))
        return failure("App does not match its SHA-256", USER_EIO);
    if (!elf_valid(update.buffer + package.program_offset, package.program_size))
        return failure("Program not for this system", USER_EINVAL);
    if (!system_store_sectors(SYSTEM_CACHE)) return failure(storage_failed, USER_EIO);
    const char *message;
    int32_t result = app_store_install(update.buffer, update.total, &message);
    if (result) return failure(message, result);
    drop();
    update.state = USER_UPDATE_DONE;
    say(message);
    page_host_redraw();
    return 0;
}

static int32_t finish(uint32_t pid) {
    const struct system_update_header *h = &update.header;
    if (update.owner != pid) return USER_EINVAL;
    if (update.app) return app_finish();
    uint8_t digest[32];
    if (update.state == USER_UPDATE_RECEIVING) {
        if (update.received != h->total_size) return failure("The download ended early", USER_EIO);
        sha256_end(&update.boot_hash, digest);
        if (memcmp(digest, h->boot_sha256, 32))
            return failure("Boot image does not match its SHA-256", USER_EIO);
        sha256_end(&update.system_hash, digest);
        if (memcmp(digest, h->system_sha256, 32))
            return failure("Resources do not match their SHA-256", USER_EIO);
        if (!boot_image_valid()) return failure("Boot image not for this phone", USER_EINVAL);
        if (update.tail_used) {
            memset(update.tail + update.tail_used, 0, 512 - update.tail_used);
            if (!write_sectors(update.tail, 1)) return failure(storage_failed, USER_EIO);
            update.tail_used = 0;
        }
        if (!system_store_sync()) return failure(storage_failed, USER_EIO);
        sha256_begin(&update.system_hash);
        update.checked = 0;
        update.state = USER_UPDATE_CHECKING;
        return USER_EAGAIN;
    }
    if (update.state == USER_UPDATE_CHECKING) {
        uint64_t start = time_us(), first = slot_sector(update.slot) + SYSTEM_SLOT_HEADER / 512;
        while (update.checked < h->system_size) {
            if (time_us() - start >= CHECK_US) return USER_EAGAIN;
            uint32_t left = h->system_size - update.checked,
                     n = left < sizeof(scratch) ? left : sizeof(scratch);
            if (!system_store_read(SYSTEM_CACHE, first + update.checked / 512, scratch,
                                   (n + 511) / 512))
                return failure(storage_failed, USER_EIO);
            sha256_add(&update.system_hash, scratch, n);
            update.checked += n;
            work();
        }
        sha256_end(&update.system_hash, digest);
        if (memcmp(digest, h->system_sha256, 32))
            return failure("Storage did not keep the update", USER_EIO);
        struct system_slot slot = {.magic = {'S', 'O', 'S', '2', 'S', 'L', 'O', 'T'},
                                   .version = 1,
                                   .data_offset = SYSTEM_SLOT_HEADER,
                                   .data_size = h->system_size};
        memcpy(slot.sha256, h->system_sha256, 32);
        slot.crc32 = crc32_ieee(&slot, offsetof(struct system_slot, crc32));
        memset(sector, 0, sizeof(sector));
        memcpy(sector, &slot, sizeof(slot));
        if (!system_store_write(SYSTEM_CACHE, slot_sector(update.slot), sector, 1) ||
            !system_store_sync())
            return failure(storage_failed, USER_EIO);
        /* The boot image will name this slot: its header too must be kept. */
        struct system_slot stored;
        if (!slot_read(update.slot, &stored) || memcmp(&stored, &slot, sizeof(slot)))
            return failure("Storage did not keep the update", USER_EIO);
        update.state = USER_UPDATE_WRITING;
        return USER_EAGAIN;
    }
    if (update.state == USER_UPDATE_WRITING) {
        uint32_t sectors = h->boot_size / 512;
        /* From the first write until the last, no release starts. */
        restart_due = false;
        memset(sector, 0, sizeof(sector));
        if (!system_store_write(SYSTEM_BOOT, 0, sector, 1) || !system_store_sync() ||
            !write_boot(1, sectors - 1) || !system_store_sync() || !boot_matches(1, sectors - 1) ||
            !write_boot(0, 1) || !system_store_sync() || !boot_matches(0, 1))
            return failure("Boot image write failed; flash flash-boot.img", USER_EIO);
        drop();
        update.state = USER_UPDATE_DONE;
        boot_slot = update.slot;
        restart_due = true;
        say("Installed; restart to use it");
        return 0;
    }
    return USER_EINVAL;
}

static int32_t restart(void) {
    /* The boot image is written within one FINISH: waiting for it means a
     * FINISH still to come. */
    if (update.state == USER_UPDATE_WRITING) return USER_EBUSY;
#ifdef SOS2_BOARD_REDMI2
    (void)redmi2_power_restart();
    say("Did not restart; hold Power for 10 s");
#else
    if (psci_method == CPU_BOOT_HVC || psci_method == CPU_BOOT_SMC)
        (void)arm_psci_cpu_on(psci_method, 0x84000009 /* PSCI SYSTEM_RESET */, 0, 0);
    say("This machine did not restart");
#endif
    return USER_EIO;
}

int32_t system_update_apps(struct vm_space *space, uint32_t address, uint32_t size) {
    const struct app *records[2 * APP_LIMIT];
    unsigned count = apps_records(records, 2 * APP_LIMIT), n = 0;
    if (size % sizeof(struct user_update_app)) return USER_EINVAL;
    for (; n < count && n < size / sizeof(struct user_update_app); ++n) {
        const struct app *a = records[n];
        struct user_update_app out = {.flags = (a->stored ? USER_APP_STORED : 0) |
                                               (process_app_running(a) ? USER_APP_RUNNING : 0) |
                                               (a->stale ? USER_APP_STALE : 0),
                                      .abi = a->stored ? a->abi : USER_ABI_VERSION};
        memcpy(out.id, a->id, sizeof(a->id));
        memcpy(out.name, a->name, sizeof(a->name));
        memcpy(out.sha256, a->sha256, sizeof(out.sha256));
        if (!vm_copy_to_user(space, address + n * sizeof(out), &out, sizeof(out)))
            return USER_EFAULT;
    }
    return (int32_t)n;
}

static int32_t remove_app(struct vm_space *space, uint32_t address, uint32_t size) {
    char id[16];
    if (!update.installs) return USER_ENODEV;
    if (update.owner) return USER_EBUSY;
    if (size != sizeof(id)) return USER_EINVAL;
    if (!vm_copy_from_user(space, id, address, size)) return USER_EFAULT;
    if (!memchr(id, 0, sizeof(id))) return USER_EINVAL;
    const char *message;
    int32_t result = app_store_remove(id, &message);
    if (!result) {
        /* Its files go with it, so a later install starts empty. */
        char profile[FS_PROFILE_MAX] = "apps/";
        strcpy(profile + 5, id);
        if (!fs_profile_remove(profile)) message = "Removed; files not deleted";
    }
    update.state = result ? USER_UPDATE_FAILED : USER_UPDATE_DONE;
    say(message);
    if (!result) page_host_redraw();
    return result;
}

/* The receiving app's download failed: nothing is left to finish. */
static int32_t cancel(uint32_t pid) {
    if (!pid || update.owner != pid) return USER_EINVAL;
    drop();
    update.state = USER_UPDATE_IDLE;
    update.message[0] = 0;
    return 0;
}

int32_t system_update_call(uint32_t pid, struct vm_space *space, uint32_t op, uint32_t address,
                           uint32_t size) {
    worked_at = time_us();
    switch (op) {
        case USER_UPDATE_BEGIN: return begin(pid, space, address, size);
        case USER_UPDATE_WRITE: return receive(pid, space, address, size);
        case USER_UPDATE_FINISH: return address || size ? USER_EINVAL : finish(pid);
        case USER_UPDATE_RESTART: return address || size ? USER_EINVAL : restart();
        case USER_UPDATE_REMOVE: return remove_app(space, address, size);
        case USER_UPDATE_CANCEL: return address || size ? USER_EINVAL : cancel(pid);
        default: return USER_EINVAL;
    }
}

void system_update_release(uint32_t pid) {
    if (!pid || update.owner != pid) return;
    (void)failure("Updater closed; update stopped", 0);
}
