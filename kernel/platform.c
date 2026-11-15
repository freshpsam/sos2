#include "platform.h"
#include "user_abi.h"
#include "graphics.h"
#include "display.h"
#include "haptics.h"

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
    /* ARM architectural generic timer: virtual count and its frequency.
     * MRRC reads both counter halves atomically, avoiding a rollover race. */
    uint32_t low, high, frequency;
    __asm__ volatile("mrrc p15, 1, %0, %1, c14" : "=r"(low), "=r"(high));
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(frequency));
    if (!frequency) panic("architected timer has zero frequency");
    return user_timer_us(((uint64_t)high << 32) | low, frequency);
}

_Noreturn void panic(const char *message) {
    /* No loop is left to end a buzz. Only CPU 0 drives the vibrator, and its
     * SPMI wait reads the timer, whose zero frequency is itself a panic. */
    uint32_t mpidr, frequency;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(frequency));
    if (!(mpidr & 0xffffff) && frequency) haptics_panic();
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
    for (;;) __asm__ volatile("wfe");
}
