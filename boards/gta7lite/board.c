#include "gta7lite.h"
#include "boot_handoff.h"
#include "board.h"
#include "platform.h"

/* MT6765's top reset generation unit: a write of the reload key to WDT_RST
 * restarts the watchdog's countdown (Linux drivers/watchdog/mtk_wdt.c). */
enum { TOPRGU = 0x10007000, WDT_RST = 0x08, WDT_RST_RELOAD = 0x1971 };

void gta7lite_watchdog_feed(void) { mmio_write32(TOPRGU + WDT_RST, WDT_RST_RELOAD); }

/* Samsung's LK starts download mode when the reboot reason in MediaTek's
 * boot-params buffer says so, as Samsung's kernel leaves it for "reboot
 * download" (drivers/samsung/sec_reboot.c). LK names the buffer, in SRAM, in
 * /chosen/ram_console: 2 KiB on this tablet. Its header (struct
 * mboot_params_buffer: "DBGC", LK's last-boot record at off_llk, sz_lk long,
 * the buffer's size at 40, the Linux stage's offset at 44) is completed by
 * Android's kernel at each boot (drivers/misc/mediatek/aee/mrdump/
 * mboot_params.c, mboot_params_init): the size, and the Linux stage's
 * struct last_reboot_reason after LK's record or 512 bytes in, cleared. A
 * restart into download mode does the same, then sets is_upload,
 * is_power_reset and power_reset_reason, 972, 980 and 984 bytes into the
 * record (include/linux/sec_debug.h, CPU_NUMS 8). */
enum {
    BOOT_PARAMS_SIGNATURE = 0x43474244,
    BOOT_PARAMS_OFF_LK = 16,
    BOOT_PARAMS_OFF_LLK = 20,
    BOOT_PARAMS_SZ_LK = 24,
    BOOT_PARAMS_SIZE = 40,
    BOOT_PARAMS_LINUX = 44,
    BOOT_PARAMS_LINUX_DEFAULT = 512,
    IS_UPLOAD = 972,
    IS_POWER_RESET = 980,
    POWER_RESET_REASON = 984,
    UPLOAD_MAGIC_NORMAL = 0,
    SEC_POWER_RESET = 0x12345678,
    SEC_RESET_REASON_UNKNOWN = 0x12345670,
    SEC_RESET_REASON_DOWNLOAD = 0x12345671
};

static uint32_t boot_params, boot_params_size;

static uint32_t boot_param(unsigned offset) { return mmio_read32(boot_params + offset); }

/* Where mboot_params_init puts the Linux stage's record: just after LK's,
 * 64-byte aligned, when LK's lies where its offsets say, else 512 bytes in.
 * 0 without a header or when the buffer cannot hold the reason. */
static uint32_t linux_stage(void) {
    if (!boot_params || boot_param(0) != BOOT_PARAMS_SIGNATURE) return 0;
    uint64_t lk = boot_param(BOOT_PARAMS_SZ_LK), lk_room = (lk + 63) & ~63ull;
    uint64_t at = lk && boot_param(BOOT_PARAMS_OFF_LK) + lk_room == boot_param(BOOT_PARAMS_OFF_LLK)
                      ? boot_param(BOOT_PARAMS_OFF_LLK) + lk_room
                      : BOOT_PARAMS_LINUX_DEFAULT;
    return at % 4 || at + POWER_RESET_REASON + 4 > boot_params_size ? 0 : (uint32_t)at;
}

bool gta7lite_download_mode_available(void) { return linux_stage() != 0; }

bool gta7lite_download_mode(bool on) {
    uint32_t at = linux_stage();
    if (!at) return false;
    uintptr_t record = boot_params + at;
    if (on) {
        mmio_write32(boot_params + BOOT_PARAMS_SIZE, boot_params_size);
        mmio_write32(boot_params + BOOT_PARAMS_LINUX, at);
        for (uint32_t i = at; i + 4 <= boot_params_size; i += 4) mmio_write32(boot_params + i, 0);
        mmio_write32(record + IS_UPLOAD, UPLOAD_MAGIC_NORMAL);
        mmio_write32(record + IS_POWER_RESET, SEC_POWER_RESET);
        mmio_write32(record + POWER_RESET_REASON, SEC_RESET_REASON_DOWNLOAD);
    } else if (boot_param(BOOT_PARAMS_LINUX) == at) {
        mmio_write32(record + POWER_RESET_REASON, SEC_RESET_REASON_UNKNOWN);
    }
    barrier();
    return true;
}

/* The panel as LK leaves it scanning out: 800x1340 ARGB8888, rows 3200
 * bytes apart. LK's handoff (atag,videolfb) gives only the framebuffer's
 * base and size. */
enum { WIDTH = 800, HEIGHT = 1340, STRIDE = 3200 };

const char *gta7lite_handoff(struct platform_devices *d, uint64_t kernel_start, uint64_t kernel_end,
                             uint64_t dtb_address) {
    if (d->has_ram_console) {
        boot_params = d->ram_console_start;
        boot_params_size = d->ram_console_size;
    }
    /* Only LK's validated scanout can be used, including for boot errors. */
    d->has_framebuffer = false;
    d->framebuffer = (struct framebuffer){0};
    d->framebuffer_size = 0;
    if (!d->has_videolfb) return "LK framebuffer handoff (atag,videolfb) missing";
    uint64_t fb = d->videolfb_base, fb_size = d->videolfb_vram;
    if (!fb || (fb & 4095) || fb_size < (uint64_t)STRIDE * HEIGHT)
        return "LK framebuffer too small or not page-aligned";
    const struct boot_handoff h = {.kernel_start = kernel_start,
                                   .kernel_end = kernel_end,
                                   .dtb = dtb_address,
                                   .fb = fb,
                                   .fb_size = fb_size,
                                   .fb_limit = 1ull << 32};
    const char *error = boot_handoff_screen(d, &h);
    if (error) return error;
    d->framebuffer =
        (struct framebuffer){(void *)(uintptr_t)fb, WIDTH, HEIGHT, STRIDE, PIXEL_ARGB8888};
    d->framebuffer_size = fb_size;
    d->has_framebuffer = true;
    error = boot_handoff_memory(d, &h);
    if (error) return error;
    /* Wi-Fi's memory: without it, Wi-Fi stays off and says so. */
    (void)gta7lite_connsys_reserve(d, kernel_start, kernel_end, dtb_address, d->dtb_size);
    return 0;
}
