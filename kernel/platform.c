#include "platform.h"
#include "user_abi.h"
#include "graphics.h"
#include "display.h"
#include "haptics.h"
#ifdef SOS2_BOARD_GTA7LITE
#include "gta7lite.h"
#endif

static struct framebuffer *panic_screen;

void platform_set_framebuffer(struct framebuffer *fb) { panic_screen = fb; }

/* Boot error text at twice the desktop's small size, so a photo of the
 * phone's panel shows every character: wrapped at spaces, and at newlines,
 * to the screen's width. Returns the y below its last line; draws only when
 * draw is set. */
static int panic_lines(int y, const char *text, bool draw) {
    enum { SCALE = 2, ADVANCE = 6 * SCALE, LINE = 10 * SCALE, COLUMNS = 120 };

    unsigned width = panic_screen->width;
    unsigned columns = width > 32 + ADVANCE ? (width - 32) / ADVANCE : 1;
    if (columns > COLUMNS) columns = COLUMNS;
    char line[COLUMNS + 1];
    while (*text && y < (int)panic_screen->height) {
        unsigned n = 0, space = 0;
        for (; text[n] && text[n] != '\n' && n < columns; ++n)
            if (text[n] == ' ') space = n;
        /* A line that ends inside a word breaks at its last space, if any. */
        if (text[n] && text[n] != '\n' && text[n] != ' ' && space) n = space;
        memcpy(line, text, n);
        line[n] = 0;
        if (draw) gfx_text(panic_screen, 16, y, line, SCALE, 0xffffff);
        y += LINE;
        text += n;
        if (*text == ' ' || *text == '\n') ++text;
    }
    return y;
}

uint64_t time_us(void) {
    /* ARM architectural generic timer: virtual count and its frequency. */
    uint64_t count = arch_counter();
    uint32_t frequency = arch_counter_frequency();
    if (!frequency) panic("architected timer has zero frequency");
    return user_timer_us(count, frequency);
}

void delay_us(uint64_t us) {
    for (uint64_t start = time_us(); time_us() - start < us;) { }
}

_Noreturn void panic(const char *message) {
    /* No loop is left to end a buzz. Only CPU 0 drives the vibrator, and its
     * SPMI wait reads the timer, whose zero frequency is itself a panic. */
    uint32_t affinity = arch_cpu_affinity(), frequency = arch_counter_frequency();
    if (!affinity && frequency) haptics_panic();
    if (panic_screen) {
        static const char stopped[] = "Boot stopped. Restart the device.";
        int bottom = panic_lines(panic_lines(52, message, false) + 12, stopped, false);
        gfx_rect(panic_screen, 0, 0, (int)panic_screen->width, (int)panic_screen->height, 0x151a23);
        gfx_rect(panic_screen, 0, 0, (int)panic_screen->width, bottom + 10, 0x801d25);
        gfx_text(panic_screen, 16, 16, "sos2 BOOT ERROR", 2, 0xffffff);
        panic_lines(panic_lines(52, message, true) + 12, stopped, true);
        barrier();
        display_flush_error();
    }
#ifdef SOS2_BOARD_GTA7LITE
    /* LK leaves the watchdog running, and its reset would take the message.
     * On CPU 0 the USB link stays up, started now if it was not yet: the
     * computer can restart the tablet, or flash a fixed system. */
    bool usb = !affinity && !gta7lite_usb_init();
    if (usb) {
        gta7lite_fastboot_panic();
        gta7lite_usb_connect();
    }
    for (;;) {
        gta7lite_watchdog_feed();
        if (usb) gta7lite_usb_poll();
        for (uint64_t at = arch_counter(); arch_counter() - at < frequency / 1000;) { }
    }
#else
    for (;;) arch_wait_event();
#endif
}
