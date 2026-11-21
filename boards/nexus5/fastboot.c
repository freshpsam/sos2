#include "nexus5.h"
#include "kernel_io.h"
#include "platform.h"
#include "system_store.h"
#include "system_update.h"

/* The Nexus 5's side of fastboot over USB (usb.c), with Android's fastboot
 * tool on the computer it is plugged into:
 *   fastboot -s nexus5 getvar all
 *   fastboot -s nexus5 oem stage-boot   then: fastboot -s nexus5 get_staged FILE,
 *                                       the boot partition, read as it goes
 *                                       out (a backup of Android's before
 *                                       installing)
 *   fastboot -s nexus5 reboot
 *   fastboot -s nexus5 reboot-bootloader   the stock bootloader's fastboot
 * The protocol is AOSP's fastboot README: one command per bulk OUT
 * transfer, answered by INFO lines and then OKAY or FAIL; get_staged's
 * upload is answered by DATA and its size, then the bytes, then OKAY.
 * Commands run in the kernel's main loop, or in a panic's
 * (kernel/platform.c). While a system update writes the boot partition,
 * nothing restarts the phone or reads the partition; after a panic, the
 * restarts always run, and the eMMC only if the service worker has no job
 * left waiting, which a transfer's yield would resume into the loop the
 * panic ended. */

enum { RESPONSE = 63, TEXT = RESPONSE - 4, PIECE = 16384 };

static uint32_t staged; /* the boot partition's bytes once oem stage-boot found it */
static bool stopped;    /* after a panic */

/* kind ("INFO", "OKAY", "FAIL" or "DATA") and its text, cut to fit. */
static bool reply(const char *kind, const char *text) {
    char packet[RESPONSE + 1];
    char *p = stpcpy(packet, kind);
    for (unsigned room = TEXT; *text && room; --room) *p++ = *text++;
    return nexus5_usb_send(packet, (uint32_t)(p - packet));
}

static bool starts(const char *text, const char *prefix) {
    size_t n = strlen(prefix);
    return strlen(text) >= n && !memcmp(text, prefix, n);
}

static void getvar(const char *name) {
    if (!strcmp(name, "version")) (void)reply("OKAY", "0.4");
    else if (!strcmp(name, "product") || !strcmp(name, "serialno")) (void)reply("OKAY", "nexus5");
    else if (!strcmp(name, "all")) {
        if (reply("INFO", "version: 0.4") && reply("INFO", "product: nexus5") &&
            reply("INFO", "serialno: nexus5"))
            (void)reply("OKAY", "");
    } else (void)reply("FAIL", "unknown variable");
}

/* Why the eMMC is not the main loop's now, or NULL. */
static const char *storage_busy(void) {
    if (system_update_busy()) return "a system update is being written";
    if (stopped && kernel_io_busy()) return "storage is busy; restart and try again";
    return nexus5_emmc_idle() ? 0 : "eMMC busy: try again";
}

/* oem stage-boot: reads only, the card and its GPT as the installed system
 * finds them. */
static void stage_boot(void) {
    staged = 0;
    const char *busy = storage_busy();
    if (busy) (void)reply("FAIL", busy);
    else if (!system_store_init(nexus5_devices()) || !system_store_sectors(SYSTEM_BOOT))
        (void)reply("FAIL", "no boot partition");
    else {
        staged = (uint32_t)(system_store_sectors(SYSTEM_BOOT) * 512);
        (void)reply("OKAY", "");
    }
}

/* get_staged: the boot partition as one stream of whole 16 KiB transfers,
 * each read just before it goes (a short packet before the end overflows
 * the host's read). A failed read ends the stream short, which the host
 * reports. */
static void upload(void) {
    static uint8_t piece[PIECE] __attribute__((aligned(4)));
    if (!staged) {
        (void)reply("FAIL", "nothing staged: run oem stage-boot first");
        return;
    }
    const char *busy = storage_busy();
    if (busy) {
        (void)reply("FAIL", busy);
        return;
    }
    static const char digits[] = "0123456789abcdef";
    char data[9];
    for (unsigned i = 0; i < 8; ++i) data[i] = digits[staged >> (28 - 4 * i) & 15];
    data[8] = 0;
    if (!reply("DATA", data)) return;
    for (uint32_t at = 0; at < staged;) {
        uint32_t n = staged - at < PIECE ? staged - at : PIECE;
        if (!system_store_read(SYSTEM_BOOT, at / 512, piece, n / 512) || !nexus5_usb_send(piece, n))
            return;
        at += n;
    }
    (void)reply("OKAY", "");
}

void nexus5_fastboot_panic(void) { stopped = true; }

void nexus5_fastboot(const char *command, unsigned length) {
    if (strlen(command) != length) (void)reply("FAIL", "not a command");
    else if (starts(command, "getvar:")) getvar(command + 7);
    else if (!strcmp(command, "oem stage-boot")) stage_boot();
    else if (!strcmp(command, "upload")) upload();
    else if (!strcmp(command, "reboot") || !strcmp(command, "reboot-bootloader")) {
        /* The host has the OKAY before the restart. */
        if (system_update_busy() && !stopped)
            (void)reply("FAIL", "a system update is being written");
        else if (reply("OKAY", "")) nexus5_restart(!strcmp(command, "reboot-bootloader"));
    } else (void)reply("FAIL", "unknown command");
}
