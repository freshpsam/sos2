#include "gta7lite.h"
#include "fastboot.h"
#include "kernel_io.h"
#include "platform.h"
#include "system_update.h"

/* The tablet's own side of fastboot (kernel/usb/fastboot.c) over usb.c:
 *   fastboot getvar all            the board and the running release
 *   fastboot flash boot flash-boot.img   the installed system's image
 *   fastboot reboot
 *   fastboot reboot bootloader     Samsung's download mode, for Heimdall
 * A download is held in RAM until flash writes it as the updater writes an
 * update (kernel/protection/system_update.c). In a CPU 0 panic's loop
 * (kernel/platform.c) the service worker cannot be resumed: there a flash
 * needs it idle. Nothing restarts while a system update is being written. */

enum { TEXT = 59 }; /* a reply's text */

static bool quiet;
static uint64_t progress_at;

static const char *variable(const char *name) {
    static char text[TEXT + 1];
    if (!strcmp(name, "version-os")) return system_update_running_release();
    if (!strcmp(name, "is-userspace") || !strcmp(name, "has-slot:boot") ||
        !strcmp(name, "is-logical:boot"))
        return "no";
    if (!strcmp(name, "partition-type:boot")) return "raw";
    uint32_t bytes = !strcmp(name, "max-download-size")     ? system_update_boot_capacity()
                     : !strcmp(name, "partition-size:boot") ? system_update_boot_partition()
                                                            : 0;
    if (!bytes) return 0;
    fastboot_hex32(stpcpy(text, "0x"), bytes);
    return text;
}

static const char *const variables[] = {"version-os", "is-userspace", "max-download-size",
                                        "partition-size:boot", 0};

/* download:SIZE, eight hex digits: RAM for the image, DATA, then its bytes. */
static void download(const char *hex) {
    uint32_t size = 0;
    unsigned digits = 0;
    for (; hex[digits]; ++digits) {
        char c = hex[digits];
        unsigned v = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                     : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                     : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10)
                                            : 16;
        if (v == 16 || digits == 8) break;
        size = size << 4 | v;
    }
    if (digits != 8 || hex[digits]) {
        (void)fastboot_reply("FAIL", "download needs eight hex digits");
        return;
    }
    const char *error = system_update_boot_begin(size);
    if (error) {
        (void)fastboot_reply("FAIL", error);
        return;
    }
    char data[9];
    fastboot_hex32(data, size);
    if (!fastboot_reply("DATA", data)) return;
    error = system_update_boot_receive(gta7lite_usb_read);
    (void)fastboot_reply(error ? "FAIL" : "OKAY", error ? error : "");
}

/* No eMMC transfer may be cut into: the service worker's job finishes first,
 * within 10 s, or after a panic must not be running at all. */
static bool storage_free(void) {
    if (fastboot_stopped()) return !kernel_io_busy();
    for (uint64_t start = time_us(); kernel_io_busy(); kernel_io_step()) {
        gta7lite_watchdog_feed();
        if (time_us() - start >= 10000000) return false;
    }
    return true;
}

/* A line a second while the image is written and read back; none once the
 * host stops reading, which does not stop the install. */
static void progress(uint32_t done, uint32_t total, bool checking) {
    gta7lite_watchdog_feed();
    uint64_t now = time_us();
    if (quiet || now - progress_at < 1000000) return;
    progress_at = now;
    char line[TEXT + 1];
    char *p = stpcpy(line, checking ? "checking " : "writing ");
    p = decimal(p, done >> 20);
    p = stpcpy(p, " of ");
    p = decimal(p, (total + (1u << 20) - 1) >> 20);
    stpcpy(p, " MiB");
    quiet = !fastboot_reply("INFO", line);
}

static void flash(const char *partition) {
    if (strcmp(partition, "boot")) {
        (void)fastboot_reply("FAIL", "only boot can be flashed");
        return;
    }
    if (!storage_free()) {
        (void)fastboot_reply("FAIL", "storage is busy; restart and try again");
        return;
    }
    quiet = false;
    progress_at = time_us();
    const char *error = system_update_boot_install(progress);
    (void)fastboot_reply(error ? "FAIL" : "OKAY", error ? error : "");
}

static bool command(const char *command) {
    if (fastboot_starts(command, "download:")) download(command + 9);
    else if (fastboot_starts(command, "flash:")) flash(command + 6);
    else return false;
    return true;
}

static const char *can_restart(bool download) {
    if (download && !gta7lite_download_mode_available()) return "no download mode on this boot";
    return system_update_busy() ? "a system update is being written" : 0;
}

static void restart(bool download) {
    gta7lite_usb_drain();
    (void)system_update_restart(download);
}

static const struct fastboot_device device = {.send = gta7lite_usb_send,
                                              .product = "gta7lite",
                                              .name = "SOS2 tablet",
                                              .variable = variable,
                                              .variables = variables,
                                              .command = command,
                                              .can_restart = can_restart,
                                              .restart = restart};

void gta7lite_fastboot_init(void) { fastboot_init(&device); }
