#include "nexus5.h"
#include "fastboot.h"
#include "kernel_io.h"
#include "platform.h"
#include "system_store.h"
#include "system_update.h"

/* The Nexus 5's own side of fastboot (kernel/usb/fastboot.c) over usb.c:
 *   fastboot -s nexus5 getvar all
 *   fastboot -s nexus5 oem stage-boot   then: fastboot -s nexus5 get_staged FILE,
 *                                       the boot partition, read as it goes
 *                                       out (a backup of Android's before
 *                                       installing)
 *   fastboot -s nexus5 reboot
 *   fastboot -s nexus5 reboot-bootloader   the stock bootloader's fastboot
 * get_staged's upload is answered by DATA and its size, then the bytes, then
 * OKAY. While a system update writes the boot partition, nothing restarts
 * the phone or reads the partition; after a panic, the restarts always run,
 * and the eMMC only if the service worker has no job left waiting, which a
 * transfer's yield would resume into the loop the panic ended. */

enum { PIECE = 16384 };

static uint32_t staged; /* the boot partition's bytes once oem stage-boot found it */

/* Why the eMMC is not the main loop's now, or NULL. */
static const char *storage_busy(void) {
    if (system_update_busy()) return "a system update is being written";
    if (fastboot_stopped() && kernel_io_busy()) return "storage is busy; restart and try again";
    return nexus5_emmc_idle() ? 0 : "eMMC busy: try again";
}

/* oem stage-boot: reads only, the card and its GPT as the installed system
 * finds them. */
static void stage_boot(void) {
    staged = 0;
    const char *busy = storage_busy();
    if (busy) (void)fastboot_reply("FAIL", busy);
    else if (!system_store_init(nexus5_devices()) || !system_store_sectors(SYSTEM_BOOT))
        (void)fastboot_reply("FAIL", "no boot partition");
    else {
        staged = (uint32_t)(system_store_sectors(SYSTEM_BOOT) * 512);
        (void)fastboot_reply("OKAY", "");
    }
}

/* get_staged: the boot partition as one stream of whole 16 KiB transfers,
 * each read just before it goes (a short packet before the end overflows
 * the host's read). A failed read ends the stream short, which the host
 * reports. */
static void upload(void) {
    static uint8_t piece[PIECE] __attribute__((aligned(4)));
    if (!staged) {
        (void)fastboot_reply("FAIL", "nothing staged: run oem stage-boot first");
        return;
    }
    const char *busy = storage_busy();
    if (busy) {
        (void)fastboot_reply("FAIL", busy);
        return;
    }
    char data[9];
    fastboot_hex32(data, staged);
    if (!fastboot_reply("DATA", data)) return;
    for (uint32_t at = 0; at < staged;) {
        uint32_t n = staged - at < PIECE ? staged - at : PIECE;
        if (!system_store_read(SYSTEM_BOOT, at / 512, piece, n / 512) || !nexus5_usb_send(piece, n))
            return;
        at += n;
    }
    (void)fastboot_reply("OKAY", "");
}

static bool command(const char *command) {
    if (!strcmp(command, "oem stage-boot")) stage_boot();
    else if (!strcmp(command, "upload")) upload();
    else return false;
    return true;
}

static const char *can_restart(bool bootloader) {
    (void)bootloader;
    return system_update_busy() && !fastboot_stopped() ? "a system update is being written" : 0;
}

static void restart(bool bootloader) { nexus5_restart(bootloader); }

static const struct fastboot_device device = {.send = nexus5_usb_send,
                                              .product = "nexus5",
                                              .name = "Nexus 5",
                                              .command = command,
                                              .can_restart = can_restart,
                                              .restart = restart};

void nexus5_fastboot_init(void) { fastboot_init(&device); }
