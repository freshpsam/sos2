#include "system_update.h"
#include "system_store.h"
#include "app_store.h"
#include "page_host.h"
#include "process.h"
#include "boot_bundle.h"
#include "elf_load.h"
#include "fs.h"
#include "runtime_files.h"
#include "kernel_io.h"
#include "sha256.h"
#include "platform.h"
#include "wire.h"
#include <stddef.h>
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_power.h"
#elif defined(SOS2_BOARD_GTA7LITE)
#include "gta7lite.h"
#elif defined(SOS2_BOARD_NEXUS5)
#include "nexus5.h"
#endif
#if defined(SOS2_BOARD_REDMI2) || defined(SOS2_BOARD_GTA7LITE) || defined(SOS2_BOARD_NEXUS5)
#define SYSTEM_INSTALLS 1 /* QEMU's machines install nothing */
#endif

/* The formats are in include/system_store.h, written by
 * tools/package_redmi2.py, tools/package_gta7lite.py and
 * tools/package_nexus5.py. A system update is a whole boot image: kernel,
 * device tree and compressed resources. It arrives in RAM and is checked
 * against its SHA-256 and the shape the device's loader starts (lk2nd on the
 * phone, Samsung's LK on the tablet, the stock bootloader on the Nexus 5);
 * then the boot partition's first sector, which the loader needs to start an
 * image, is cleared, the rest is written and read back, and the first sector
 * is written last. Until it is cleared the old image is untouched; from then
 * until the last write, which a closed updater does not stop, an
 * interruption leaves no image to start: the phone falls back to lk2nd's
 * fastboot, the tablet to download mode, the Nexus 5 to its bootloader's
 * fastboot. An app's package
 * (include/app_store.h) arrives whole in RAM, is checked, and goes to its app
 * store position. Nothing is signed: the SHA-256s guard against damage, not
 * against whoever publishes the files. */
enum {
    ANDROID_PAGE = 2048,
#ifdef SOS2_BOARD_NEXUS5
    KERNEL_ADDRESS = 0x00008000,
    TAGS_ADDRESS = 0x02700000,
#else
    KERNEL_ADDRESS = 0x80008000,
    TAGS_ADDRESS = 0x81e00000,
#endif
    /* The boot image's RAM, in pieces the page allocator finds. */
    IMAGE_CHUNK = 1024 * 1024,
    IMAGE_CHUNKS = (SYSTEM_BOOT_LIMIT + IMAGE_CHUNK - 1) / IMAGE_CHUNK,
    WORK_US = 2000,  /* of eMMC or hashing work before the desktop runs */
    STEP_US = 250000 /* of writing or reading back per FINISH, between app updates */
};

#ifdef SOS2_BOARD_GTA7LITE
static const char board[16] = "gta7litewifi";
#elif defined(SOS2_BOARD_NEXUS5)
static const char board[16] = "lge-hammerhead";
#else
static const char board[16] = "redmi2-wt88047";
#endif
_Static_assert(sizeof(board) == sizeof(((struct user_update_info *)0)->board), "INFO's board");

static struct {
    uint32_t state, owner; /* owner: the receiving process */
    bool installs;
    struct system_update_header header;
    struct sha256 hash;
    unsigned char *buffer; /* an app's whole package */
    unsigned pages;
    unsigned char *image[IMAGE_CHUNKS]; /* the boot image */
    bool app;                           /* receiving an app's package */
    uint32_t received, total, done;     /* bytes of the file; of the image written or read back */
    char message[64];
} update;

static char release[SYSTEM_RELEASE_MAX + 1];
static bool restart_due; /* an installed system update waits for a restart */
#ifdef SYSTEM_INSTALLS
static bool installed; /* started from the installed system */
#endif
#if !defined(SOS2_BOARD_REDMI2) && !defined(SOS2_BOARD_NEXUS5)
static enum cpu_boot_method psci_method; /* QEMU and the tablet restart through PSCI */
#endif
static uint64_t worked_at;
/* The service worker is serialized, so one buffer serves every request. */
static uint32_t scratch[1024], sector[128];

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

void system_update_init(const struct platform_devices *d) {
#if !defined(SOS2_BOARD_REDMI2) && !defined(SOS2_BOARD_NEXUS5)
    psci_method = d->psci_method;
#endif
#ifdef SYSTEM_INSTALLS
    /* The installed system's boot image says so; the cache partition holds
     * its app store and data volume. Without the eMMC it runs as a RAM
     * session. */
    installed = storage_installed(d->bootargs, sizeof(d->bootargs)) && system_store_init(d);
#endif
    release[0] = 0;
    const struct runtime_file *f = runtime_file_find("/system/release");
    if (f && f->size && f->size <= SYSTEM_RELEASE_MAX && printable(runtime_file_data(f), f->size)) {
        memcpy(release, runtime_file_data(f), f->size);
        release[f->size] = 0;
    }
    update = (__typeof__(update)){0};
#ifdef SYSTEM_INSTALLS
    /* Installed on the eMMC, it installs there; BEGIN tells a failed card. */
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
    for (unsigned i = 0; i < IMAGE_CHUNKS; ++i) {
        if (update.image[i]) vm_cpu_free(update.image[i], IMAGE_CHUNK / PAGE_SIZE);
        update.image[i] = 0;
    }
    update.owner = 0;
}

/* The boot image's bytes from offset on, to the end of their piece. */
static unsigned char *image_at(uint32_t offset) {
    return update.image[offset / IMAGE_CHUNK] + offset % IMAGE_CHUNK;
}

/* The next run of whole sectors to write or read back from offset: at most
 * eight, inside one piece. */
static unsigned image_sectors(uint32_t offset) {
    uint32_t left = update.header.boot_size - offset, piece = IMAGE_CHUNK - offset % IMAGE_CHUNK;
    uint32_t n = (left < piece ? left : piece) / 512;
    return n < 8 ? n : 8;
}

/* From the first sector's clearing until the install's end, the boot
 * partition starts nothing. */
static bool boot_open(void) {
    return update.state == USER_UPDATE_WRITING || update.state == USER_UPDATE_CHECKING;
}

static int32_t failure(const char *text, int32_t error) {
    drop();
    update.state = USER_UPDATE_FAILED;
    say(text);
    return error;
}

static const char storage_failed[] = "Storage failed; restart to try again";
static const char boot_failed[] = "Boot image write failed; flash flash-boot.img";

int32_t system_update_info(struct vm_space *space, user_ptr address, uint32_t size) {
    struct user_update_info info = {.state = update.state,
                                    .flags = (update.installs ? USER_UPDATE_INSTALLS : 0) |
                                             (restart_due ? USER_UPDATE_RESTART_DUE : 0)};
#ifdef SOS2_BOARD_GTA7LITE
    if (gta7lite_download_mode_available()) info.flags |= USER_UPDATE_DOWNLOAD_MODE;
#endif
    if (update.state == USER_UPDATE_RECEIVING) {
        info.done = update.received;
        info.total = update.total;
    } else if (boot_open()) {
        info.done = update.done;
        info.total = update.header.boot_size;
    }
    memcpy(info.release, release, sizeof(release));
    memcpy(info.message, update.message, sizeof(update.message));
    memcpy(info.board, board, sizeof(board));
    if (size != sizeof(info)) return USER_EINVAL;
    return vm_copy_to_user(space, address, &info, sizeof(info)) ? 0 : USER_EFAULT;
}

static bool header_valid(const struct system_update_header *h) {
    const char *end = memchr(h->release, 0, sizeof(h->release));
    size_t length = end ? (size_t)(end - h->release) : 0;
    for (size_t i = length; end && i < sizeof(h->release); ++i)
        if (h->release[i]) return false;
    return crc32_ieee(h, offsetof(struct system_update_header, crc32)) == h->crc32 &&
           !memcmp(h->magic, "SOS2UPDT", 8) && h->version == 2 &&
           h->header_size == SYSTEM_UPDATE_HEADER && !memcmp(h->board, board, sizeof(board)) &&
           length && printable(h->release, length) && h->boot_offset == SYSTEM_UPDATE_HEADER &&
           h->boot_size > ANDROID_PAGE && h->boot_size <= SYSTEM_BOOT_LIMIT &&
           !(h->boot_size % ANDROID_PAGE) && h->total_size == h->boot_offset + h->boot_size;
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

static int32_t begin(uint32_t pid, struct vm_space *space, user_ptr address, uint32_t size) {
    /* Only the installed system takes files, to install them. */
    if (!update.installs) return USER_ENODEV;
    if (boot_open() || (update.state == USER_UPDATE_RECEIVING && update.owner != pid))
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
        return failure("Not an SOS2 update for this device", USER_EINVAL);
    if (update.header.boot_size > system_store_sectors(SYSTEM_BOOT) * 512)
        return failure("Boot image does not fit", USER_EFBIG);
    for (uint32_t at = 0; at < update.header.boot_size; at += IMAGE_CHUNK)
        if (!(update.image[at / IMAGE_CHUNK] = vm_cpu_alloc(IMAGE_CHUNK / PAGE_SIZE)))
            return failure("Not enough memory for the update", USER_ENOMEM);
    sha256_begin(&update.hash);
    update.app = false;
    update.total = update.header.total_size;
    update.received = SYSTEM_UPDATE_HEADER;
    update.done = 0;
    update.owner = pid;
    update.state = USER_UPDATE_RECEIVING;
    return 0;
}

/* The file's bytes after its header: the boot image, into RAM. */
static void consume(const unsigned char *p, uint32_t n) {
    sha256_add(&update.hash, p, n);
    while (n) {
        uint32_t at = update.received - SYSTEM_UPDATE_HEADER, take = IMAGE_CHUNK - at % IMAGE_CHUNK;
        if (take > n) take = n;
        memcpy(image_at(at), p, take);
        update.received += take;
        p += take;
        n -= take;
    }
}

static int32_t receive(uint32_t pid, struct vm_space *space, user_ptr address, uint32_t size) {
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
            consume((const unsigned char *)scratch, n);
        }
        done += n;
        work();
    }
    return 0;
}

#ifndef SOS2_BOARD_GTA7LITE
/* An image the phone's loader (lk2nd on the Redmi 2, the stock bootloader
 * on the Nexus 5) starts as the installed system: the kernel at its address
 * with the DTB after it, the compressed resources as the ramdisk, where the
 * loader puts one, no second stage, and a command
 * line that keeps data on the eMMC, parsed as the next boot will: it fits
 * the 255 bytes of sos2.* arguments the kernel keeps, and nothing else may
 * join it. */
static bool boot_image_valid(void) {
    const unsigned char *image = update.image[0];
    uint32_t kernel = le32(image + 8), ramdisk = le32(image + 16);
    uint64_t at =
        ANDROID_PAGE + ((uint64_t)kernel + ANDROID_PAGE - 1) / ANDROID_PAGE * ANDROID_PAGE;
    if (memcmp(image, "ANDROID!", 8) || kernel < 64 || le32(image + 12) != KERNEL_ADDRESS ||
        ramdisk <= BOOT_BUNDLE_HEADER || le32(image + 20) != BOOT_RAMDISK_BASE || le32(image + 24) ||
        le32(image + 32) != TAGS_ADDRESS || le32(image + 36) != ANDROID_PAGE ||
        at + ((uint64_t)ramdisk + ANDROID_PAGE - 1) / ANDROID_PAGE * ANDROID_PAGE !=
            update.header.boot_size ||
        le32(image + ANDROID_PAGE + 0x24) != 0x016f2818)
        return false;
    /* The ramdisk starts a page, inside one piece: a SORZ header of its size. */
    const unsigned char *resources = image_at((uint32_t)at);
    if (le32(resources) != BOOT_BUNDLE_MAGIC || le32(resources + 4) != BOOT_BUNDLE_VERSION ||
        le32(resources + 8) != BOOT_BUNDLE_HEADER ||
        le32(resources + 12) != ramdisk - BOOT_BUNDLE_HEADER)
        return false;
    const char *cmdline = (const char *)image + 64, *end = memchr(cmdline, 0, 512);
    for (unsigned i = 608; i < 608 + 1024; ++i) /* the extra command line */
        if (image[i]) return false;
    return end && (size_t)(end - cmdline) < sizeof(((struct platform_devices *)0)->bootargs) &&
           storage_installed(cmdline, 512);
}
#endif

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

/* A checked boot image in RAM: its first sector cleared, so that from now
 * until the last write no release starts, then WRITING. */
static int32_t install_begin(void) {
    restart_due = false;
    update.state = USER_UPDATE_WRITING;
    update.done = 512;
    memset(sector, 0, sizeof(sector));
    if (!system_store_write(SYSTEM_BOOT, 0, sector, 1) || !system_store_sync())
        return failure(boot_failed, USER_EIO);
    return USER_EAGAIN;
}

/* Up to STEP_US of writing, then of reading back, then the first sector:
 * USER_EAGAIN until the image is installed. */
static int32_t install_step(void) {
    const struct system_update_header *h = &update.header;
    uint64_t start = time_us();
    if (update.state == USER_UPDATE_WRITING) {
        while (update.done < h->boot_size) {
            if (time_us() - start >= STEP_US) return USER_EAGAIN;
            unsigned n = image_sectors(update.done);
            if (!system_store_write(SYSTEM_BOOT, update.done / 512, image_at(update.done), n))
                return failure(boot_failed, USER_EIO);
            update.done += n * 512;
            work();
        }
        if (!system_store_sync()) return failure(boot_failed, USER_EIO);
        update.state = USER_UPDATE_CHECKING;
        update.done = 512;
        return USER_EAGAIN;
    }
    if (update.state == USER_UPDATE_CHECKING) {
        while (update.done < h->boot_size) {
            if (time_us() - start >= STEP_US) return USER_EAGAIN;
            unsigned n = image_sectors(update.done);
            if (!system_store_read(SYSTEM_BOOT, update.done / 512, scratch, n) ||
                memcmp(scratch, image_at(update.done), n * 512))
                return failure(boot_failed, USER_EIO);
            update.done += n * 512;
            work();
        }
        /* The first sector last: from here the loader starts the new image. */
        if (!system_store_write(SYSTEM_BOOT, 0, update.image[0], 1) || !system_store_sync() ||
            !system_store_read(SYSTEM_BOOT, 0, scratch, 1) || memcmp(scratch, update.image[0], 512))
            return failure(boot_failed, USER_EIO);
        drop();
        update.state = USER_UPDATE_DONE;
        restart_due = true;
        say("Installed; restart to use it");
        return 0;
    }
    return USER_EINVAL;
}

#ifdef SOS2_BOARD_GTA7LITE
static bool tablet_update_valid(void);
#endif

static int32_t finish(uint32_t pid) {
    const struct system_update_header *h = &update.header;
    if (update.owner != pid) return USER_EINVAL;
    if (update.app) return app_finish();
    if (update.state == USER_UPDATE_RECEIVING) {
        uint8_t digest[32];
        if (update.received != h->total_size) return failure("The download ended early", USER_EIO);
        sha256_end(&update.hash, digest);
        if (memcmp(digest, h->boot_sha256, 32))
            return failure("Boot image does not match its SHA-256", USER_EIO);
#ifdef SOS2_BOARD_GTA7LITE
        if (!tablet_update_valid()) return failure("Boot image not for this device", USER_EINVAL);
#else
        if (!boot_image_valid()) return failure("Boot image not for this device", USER_EINVAL);
#endif
        return install_begin();
    }
    return install_step();
}

static int32_t restart(void) {
    /* Until the boot image's last write, the boot partition starts nothing. */
    if (boot_open()) return USER_EBUSY;
#ifdef SOS2_BOARD_REDMI2
    (void)redmi2_power_restart();
    say("Did not restart; hold Power for 10 s");
#elif defined(SOS2_BOARD_NEXUS5)
    nexus5_restart(false);
#else
    if (psci_method == CPU_BOOT_HVC || psci_method == CPU_BOOT_SMC)
        (void)arm_psci_call(psci_method, 0x84000009 /* PSCI SYSTEM_RESET */, 0, 0);
    say("This machine did not restart");
#endif
    return USER_EIO;
}

/* Samsung's download mode, which the tablet's LK starts when the reboot
 * reason asks for it; cleared again if the restart does not happen. */
static int32_t restart_download(void) {
#ifdef SOS2_BOARD_GTA7LITE
    if (boot_open()) return USER_EBUSY;
    if (!gta7lite_download_mode(true)) return USER_ENODEV;
    int32_t result = restart();
    (void)gta7lite_download_mode(false);
    return result;
#else
    return USER_ENODEV;
#endif
}

bool system_update_busy(void) { return boot_open(); }

#ifdef SOS2_BOARD_GTA7LITE
/* fastboot's downloads (boards/gta7lite/fastboot.c) hold the update in place
 * of a process, which no pid matches. */
enum { FASTBOOT_OWNER = UINT32_MAX, TABLET_KERNEL = 0x40080000, TABLET_HEADER = 1660 };

static uint32_t page_align(uint32_t n) { return (n + ANDROID_PAGE - 1) & ~(uint32_t)(ANDROID_PAGE - 1); }

/* The image's bytes at offset, which may cross from one piece to the next. */
static bool image_is(uint32_t offset, const void *bytes, uint32_t n) {
    const unsigned char *b = bytes;
    for (uint32_t i = 0; i < n; ++i)
        if (*image_at(offset + i) != b[i]) return false;
    return true;
}

/* tools/package_gta7lite.py's installed image: an Android boot image header
 * v2 with 2 KiB pages; a gzip kernel at LK's address; the compressed
 * resources as the ramdisk, a SORZ header of its size; no second stage or
 * recovery DTBO; a device tree or DT table; Samsung's trailer ending the
 * image. Its command line keeps LK's 64-bit switch and the installed
 * system's storage. */
static bool tablet_image_valid(uint32_t size) {
    static const unsigned char gzip[2] = {0x1f, 0x8b}, fdt[4] = {0xd0, 0x0d, 0xfe, 0xed},
                               dt_table[4] = {0xd7, 0xb7, 0xab, 0x1e};
    const unsigned char *h = update.image[0];
    uint32_t kernel = le32(h + 8), ramdisk = le32(h + 16);
    uint32_t dtb = le32(h + 1648);
    if (memcmp(h, "ANDROID!", 8) || kernel < 64 || kernel > SYSTEM_BOOT_LIMIT ||
        le32(h + 12) != TABLET_KERNEL || ramdisk <= BOOT_BUNDLE_HEADER ||
        ramdisk > SYSTEM_BOOT_LIMIT || le32(h + 24) || le32(h + 36) != ANDROID_PAGE ||
        le32(h + 40) != 2 || le32(h + 1632) || le32(h + 1644) != TABLET_HEADER || !dtb ||
        dtb > SYSTEM_BOOT_LIMIT)
        return false;
    uint64_t ramdisk_at = ANDROID_PAGE + (uint64_t)page_align(kernel);
    uint64_t dtb_at = ramdisk_at + page_align(ramdisk);
    if (dtb_at + page_align(dtb) + 16 != size || !image_is(ANDROID_PAGE, gzip, 2) ||
        !image_is(size - 16, "SEANDROIDENFORCE", 16) ||
        !(image_is((uint32_t)dtb_at, fdt, 4) || image_is((uint32_t)dtb_at, dt_table, 4)))
        return false;
    /* The ramdisk starts a page, so its header lies inside one piece. */
    const unsigned char *resources = image_at((uint32_t)ramdisk_at);
    if (le32(resources) != BOOT_BUNDLE_MAGIC || le32(resources + 4) != BOOT_BUNDLE_VERSION ||
        le32(resources + 8) != BOOT_BUNDLE_HEADER ||
        le32(resources + 12) != ramdisk - BOOT_BUNDLE_HEADER)
        return false;
    const char *cmdline = (const char *)h + 64;
    if (!memchr(cmdline, 0, 512) || !storage_installed(cmdline, 512)) return false;
    static const char bootopt[] = "bootopt=64S3,32N2,64N2";
    size_t at = 0, n;
    for (const char *t; (t = token(cmdline, 512, &at, &n));)
        if (n == sizeof(bootopt) - 1 && !memcmp(t, bootopt, n)) return true;
    return false;
}

/* A downloaded update's image (sos2-gta7lite.update): header_valid wants
 * whole 2 KiB pages, so the image, which Samsung's trailer ends 16 bytes
 * into a page, comes padded with zeros to the next page; its own header
 * gives its end. */
static bool tablet_update_valid(void) {
    const unsigned char *h = update.image[0];
    uint32_t kernel = le32(h + 8), ramdisk = le32(h + 16), dtb = le32(h + 1648);
    if (kernel > SYSTEM_BOOT_LIMIT || ramdisk > SYSTEM_BOOT_LIMIT || dtb > SYSTEM_BOOT_LIMIT) return false;
    uint64_t size = ANDROID_PAGE + (uint64_t)page_align(kernel) + page_align(ramdisk) + page_align(dtb) + 16;
    if (size > update.header.boot_size || page_align((uint32_t)size) != update.header.boot_size)
        return false;
    for (uint32_t at = (uint32_t)size; at < update.header.boot_size; ++at)
        if (*image_at(at)) return false;
    return tablet_image_valid((uint32_t)size);
}

uint32_t system_update_boot_capacity(void) {
    if (!installed) return 0;
    uint64_t bytes = system_store_sectors(SYSTEM_BOOT) * 512;
    return bytes < SYSTEM_BOOT_LIMIT ? (uint32_t)bytes : SYSTEM_BOOT_LIMIT;
}

uint32_t system_update_boot_partition(void) {
    uint64_t bytes = installed ? system_store_sectors(SYSTEM_BOOT) * 512 : 0;
    return bytes < UINT32_MAX ? (uint32_t)bytes : 0;
}

const char *system_update_boot_begin(uint32_t size) {
    if (!installed) return "not the installed system";
    if (boot_open() || update.state == USER_UPDATE_RECEIVING) return "an update is in progress";
    if (size <= ANDROID_PAGE || size > SYSTEM_BOOT_LIMIT) return "not a boot image's size";
    /* Samsung's trailer ends the image 16 bytes into a sector: it is written
     * padded with zeros to whole sectors, boot_size, while total is its size. */
    uint32_t padded = (size + 511) & ~511u;
    if (padded > system_store_sectors(SYSTEM_BOOT) * 512) return "too large for the boot partition";
    drop();
    update.state = USER_UPDATE_IDLE;
    update.message[0] = 0;
    update.header = (struct system_update_header){.boot_size = padded};
    for (uint32_t at = 0; at < padded; at += IMAGE_CHUNK)
        if (!(update.image[at / IMAGE_CHUNK] = vm_cpu_alloc(IMAGE_CHUNK / PAGE_SIZE))) {
            drop();
            return "not enough memory";
        }
    update.app = false;
    update.total = size;
    update.received = update.done = 0;
    update.owner = FASTBOOT_OWNER;
    update.state = USER_UPDATE_RECEIVING;
    return 0;
}

const char *system_update_boot_receive(bool (*read)(void *data, uint32_t size)) {
    if (update.owner != FASTBOOT_OWNER || update.state != USER_UPDATE_RECEIVING)
        return "no download begun";
    while (update.received < update.total) {
        uint32_t piece = IMAGE_CHUNK - update.received % IMAGE_CHUNK;
        if (piece > update.total - update.received) piece = update.total - update.received;
        if (!read(image_at(update.received), piece)) {
            drop();
            update.state = USER_UPDATE_IDLE;
            return "the download stopped";
        }
        update.received += piece;
    }
    /* The padding lies inside the last sector, so inside one piece. */
    if (update.header.boot_size > update.total)
        memset(image_at(update.total), 0, update.header.boot_size - update.total);
    return 0;
}

const char *system_update_boot_install(void (*progress)(uint32_t done, uint32_t total, bool checking)) {
    if (update.owner != FASTBOOT_OWNER || update.state != USER_UPDATE_RECEIVING ||
        update.received != update.total)
        return "nothing downloaded";
    if (!tablet_image_valid(update.total)) {
        (void)failure("Not an installed SOS2 image for this tablet", USER_EINVAL);
        return update.message;
    }
    int32_t result = install_begin();
    while (result == USER_EAGAIN) {
        progress(update.done, update.header.boot_size, update.state == USER_UPDATE_CHECKING);
        result = install_step();
    }
    return result ? update.message : 0;
}
#endif

int32_t system_update_restart(bool download_mode) {
    return download_mode ? restart_download() : restart();
}

const char *system_update_running_release(void) { return release; }

int32_t system_update_apps(struct vm_space *space, user_ptr address, uint32_t size) {
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

static int32_t remove_app(struct vm_space *space, user_ptr address, uint32_t size) {
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
    if (boot_open()) return USER_EBUSY;
    drop();
    update.state = USER_UPDATE_IDLE;
    update.message[0] = 0;
    return 0;
}

int32_t system_update_call(uint32_t pid, struct vm_space *space, uint32_t op, user_ptr address,
                           uint32_t size) {
    worked_at = time_us();
    switch (op) {
        case USER_UPDATE_BEGIN: return begin(pid, space, address, size);
        case USER_UPDATE_WRITE: return receive(pid, space, address, size);
        case USER_UPDATE_FINISH: return address || size ? USER_EINVAL : finish(pid);
        case USER_UPDATE_RESTART: return address || size ? USER_EINVAL : restart();
        case USER_UPDATE_REMOVE: return remove_app(space, address, size);
        case USER_UPDATE_CANCEL: return address || size ? USER_EINVAL : cancel(pid);
        case USER_UPDATE_DOWNLOAD: return address || size ? USER_EINVAL : restart_download();
        default: return USER_EINVAL;
    }
}

void system_update_release(uint32_t pid) {
    if (!pid || update.owner != pid) return;
    /* Past the first sector, only the whole write leaves a phone that starts:
     * it goes on, on the service worker, without the updater. */
    if (boot_open()) {
        while (finish(pid) == USER_EAGAIN) continue;
        return;
    }
    (void)failure("Updater closed; update stopped", 0);
}
