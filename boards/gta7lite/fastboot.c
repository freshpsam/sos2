#include "gta7lite.h"
#include "kernel_io.h"
#include "platform.h"
#include "system_update.h"

/* The tablet's side of fastboot over USB (usb.c), for the computer it is
 * plugged into, with Android's fastboot tool and no driver or root:
 *   fastboot getvar all            the board and the running release
 *   fastboot flash boot flash-boot.img   the installed system's image
 *   fastboot reboot
 *   fastboot reboot bootloader     Samsung's download mode, for Heimdall
 * The protocol is AOSP's fastboot README: one command per bulk OUT
 * transfer, answered by INFO lines and then OKAY or FAIL; a download is
 * answered by DATA and its size, then sent as one transfer, then OKAY.
 * Responses stay under 64 bytes, so each is one short packet and ends its
 * transfer at either speed. A download is held in RAM until flash writes it
 * as the updater writes an update (kernel/protection/system_update.c).
 * Commands run in the kernel's main loop, or in a CPU 0 panic's
 * (kernel/platform.c), where the service worker cannot be resumed: there a
 * flash needs it idle. */

enum { RESPONSE = 63, TEXT = RESPONSE - 4 };

static bool stopped, quiet;
static uint64_t progress_at;

/* kind ("INFO", "OKAY", "FAIL" or "DATA") and its text, cut to fit. */
static bool reply(const char *kind, const char *text) {
    char packet[RESPONSE + 1];
    char *p = stpcpy(packet, kind);
    for (unsigned room = TEXT; *text && room; --room) *p++ = *text++;
    return gta7lite_usb_send(packet, (uint32_t)(p - packet));
}

static bool starts(const char *text, const char *prefix) {
    size_t n = strlen(prefix);
    return strlen(text) >= n && !memcmp(text, prefix, n);
}

static char *hex32(char *p, uint32_t value) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; ++i) *p++ = digits[value >> (28 - 4 * i) & 15];
    *p = 0;
    return p;
}

static const char *variable(const char *name) {
    static char text[TEXT + 1];
    if (!strcmp(name, "version")) return "0.4";
    if (!strcmp(name, "product") || !strcmp(name, "serialno")) return "gta7lite";
    if (!strcmp(name, "version-os")) return system_update_running_release();
    if (!strcmp(name, "is-userspace") || !strcmp(name, "has-slot:boot") ||
        !strcmp(name, "is-logical:boot"))
        return "no";
    if (!strcmp(name, "partition-type:boot")) return "raw";
    uint32_t bytes = !strcmp(name, "max-download-size")     ? system_update_boot_capacity()
                     : !strcmp(name, "partition-size:boot") ? system_update_boot_partition()
                                                            : 0;
    if (!bytes) return 0;
    hex32(stpcpy(text, "0x"), bytes);
    return text;
}

static void getvar(const char *name) {
    static const char *const names[] = {"version", "product", "serialno", "version-os",
                                        "is-userspace", "max-download-size", "partition-size:boot"};
    if (strcmp(name, "all")) {
        const char *value = variable(name);
        (void)reply(value ? "OKAY" : "FAIL", value ? value : "unknown variable");
        return;
    }
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char line[TEXT + 1];
        const char *value = variable(names[i]);
        if (!value) continue;
        unsigned n = 0;
        for (const char *s = names[i]; *s && n < TEXT; ++s) line[n++] = *s;
        for (const char *s = ": "; *s && n < TEXT; ++s) line[n++] = *s;
        for (const char *s = value; *s && n < TEXT; ++s) line[n++] = *s;
        line[n] = 0;
        if (!reply("INFO", line)) return;
    }
    (void)reply("OKAY", "");
}

/* download:SIZE, eight hex digits: RAM for the image, DATA, then its bytes. */
static void download(const char *hex) {
    uint32_t size = 0;
    unsigned digits = 0;
    for (; hex[digits]; ++digits) {
        char c = hex[digits];
        unsigned v = c >= '0' && c <= '9' ? (unsigned)(c - '0')
                     : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                     : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10)
                                            : 16;
        if (v == 16 || digits == 8) break;
        size = size << 4 | v;
    }
    if (digits != 8 || hex[digits]) {
        (void)reply("FAIL", "download needs eight hex digits");
        return;
    }
    const char *error = system_update_boot_begin(size);
    if (error) {
        (void)reply("FAIL", error);
        return;
    }
    char data[9];
    hex32(data, size);
    if (!reply("DATA", data)) return;
    error = system_update_boot_receive(gta7lite_usb_read);
    (void)reply(error ? "FAIL" : "OKAY", error ? error : "");
}

/* No eMMC transfer may be cut into: the service worker's job finishes first,
 * within 10 s, or after a panic must not be running at all. */
static bool storage_free(void) {
    if (stopped) return !kernel_io_busy();
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
    quiet = !reply("INFO", line);
}

static void flash(const char *partition) {
    if (strcmp(partition, "boot")) {
        (void)reply("FAIL", "only boot can be flashed");
        return;
    }
    if (!storage_free()) {
        (void)reply("FAIL", "storage is busy; restart and try again");
        return;
    }
    quiet = false;
    progress_at = time_us();
    const char *error = system_update_boot_install(progress);
    (void)reply(error ? "FAIL" : "OKAY", error ? error : "");
}

/* The host has the OKAY before the restart. */
static void restart(bool download) {
    if (download && !gta7lite_download_mode_available()) {
        (void)reply("FAIL", "no download mode on this boot");
        return;
    }
    if (system_update_busy()) {
        (void)reply("FAIL", "a system update is being written");
        return;
    }
    if (!reply("OKAY", "")) return;
    gta7lite_usb_drain();
    (void)system_update_restart(download);
}

void gta7lite_fastboot_panic(void) { stopped = true; }

void gta7lite_fastboot(const char *command, unsigned length) {
    if (strlen(command) != length) (void)reply("FAIL", "not a command");
    else if (starts(command, "getvar:")) getvar(command + 7);
    else if (starts(command, "download:")) download(command + 9);
    else if (starts(command, "flash:")) flash(command + 6);
    else if (!strcmp(command, "reboot")) restart(false);
    else if (!strcmp(command, "reboot-bootloader")) restart(true);
    else (void)reply("FAIL", "unknown command");
}
