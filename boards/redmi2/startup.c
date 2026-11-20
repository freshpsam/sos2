#include "redmi2_startup.h"
#include "boot_progress.h"
#include "phone_input.h"
#include "redmi2_power.h"
#include "platform.h"
#include "page_host.h"
#include "process.h"
#include "kernel_io.h"
#include "wifi_setup.h"
#include "redmi2_wifi.h"
#include "wifi_device.h"

static struct framebuffer *screen;
static uint64_t last_poll;
static int painted_phase, serviced_phase;
static unsigned painted_percent, shown_percent;
static const char *painted_detail;
static bool repaint;
/* Each phase's name, on screen while it runs. */
static const char *const phases[] = {
    "Preparing controls", "Checking browser",  "Loading browser",      "Verifying browser",
    "Preparing memory",   "Preparing storage", "Gathering randomness", "Starting network",
    "Reading clock",      "Starting Wi-Fi",
};
/* One bar for the whole start: where each phase begins, in percent, sized to
 * its usual time on the phone (about 36 s, loading 17 s and Wi-Fi 12 s; lk2nd
 * reads the image, installed or not, before the kernel starts). Phases that
 * report progress fill their share; the bar never goes back. */
static const uint8_t phase_start[] = {0, 1, 5, 52, 59, 61, 62, 66, 66, 67};
_Static_assert(sizeof(phases) / sizeof(phases[0]) == BOOT_WIFI + 1 &&
                   sizeof(phase_start) == BOOT_WIFI + 1,
               "a name and a share for every boot phase");

enum { BACKGROUND = 0x17232c, FOREGROUND = 0xf1f5f6, MUTED = 0xb9c9d0, ACCENT = 0x61e0bd };

/* The phase's name, then "...", and below the bar an optional detail line
 * (the Wi-Fi start's current step). */
static void paint(enum boot_phase phase, unsigned percent, const char *detail) {
    unsigned x = screen->width / 16, y = screen->height / 4, width = screen->width - 2 * x;
    if (repaint) {
        gfx_rect(screen, 0, 0, screen->width, screen->height, BACKGROUND);
        gfx_text(screen, x, screen->height / 10, "SOS2", 2, ACCENT);
        painted_phase = -1;
        painted_percent = UINT32_MAX;
        painted_detail = 0;
    }
    if (painted_phase != (int)phase) {
        char label[32];
        size_t n = strlen(phases[phase]);
        memcpy(label, phases[phase], n);
        memcpy(label + n, "...", 4);
        gfx_rect(screen, x, y, width, 32, BACKGROUND);
        gfx_text(screen, x, y, label, 2, FOREGROUND);
        painted_phase = (int)phase;
    }
    if (painted_percent != percent) {
        gfx_rect(screen, x, y + 48, width, 10, 0x31515c);
        gfx_rect(screen, x, y + 48, width * percent / 100, 10, ACCENT);
        gfx_rect(screen, x, y + 76, width, 20, BACKGROUND);
        char number[5];
        unsigned n = 0;
        if (percent == 100) number[n++] = '1';
        if (percent >= 10) number[n++] = (char)('0' + percent / 10 % 10);
        number[n++] = (char)('0' + percent % 10);
        number[n++] = '%';
        number[n] = 0;
        gfx_text(screen, x, y + 76, number, 2, ACCENT);
        painted_percent = percent;
    }
    if (painted_detail != detail) {
        gfx_rect(screen, x, y + 112, width, 20, BACKGROUND);
        if (detail) gfx_text(screen, x, y + 112, detail, 2, MUTED);
        painted_detail = detail;
    }
    repaint = false;
    barrier();
}

static void update(enum boot_phase phase, uint32_t done, uint32_t total) {
    if (!screen || (unsigned)phase >= sizeof(phases) / sizeof(phases[0])) return;
    uint64_t now = time_us();
    /* Checkpoints are byte-bounded; hardware polling is time-bounded. Paint
     * only changed labels/bar regions, never a full uncached frame per tick. */
    if (serviced_phase == (int)phase && now - last_poll < 8000) return;
    last_poll = now;
    serviced_phase = (int)phase;
    enum redmi2_power_change change = redmi2_power_poll();
    if (change == REDMI2_POWER_SCREEN_OFF) phone_input_suspend(0);
    else if (change == REDMI2_POWER_SCREEN_ON) {
        phone_input_resume();
        repaint = true;
    }
    if (!redmi2_power_screen_on()) return;
    /* Keep the controller serviced while loading; gestures belong to this
     * screen and must not become clicks when the desktop opens. */
    struct pointer_event event;
    (void)phone_input_poll(&event, 0);
    unsigned first = phase_start[phase], last = phase == BOOT_WIFI ? 100 : phase_start[phase + 1];
    unsigned percent =
        first +
        (total ? (unsigned)((uint64_t)(done < total ? done : total) * (last - first) / total) : 0);
    if (percent > shown_percent) shown_percent = percent;
    paint(phase, shown_percent, phase == BOOT_WIFI ? wifi_device_status() : 0);
}

void redmi2_startup_begin(struct framebuffer *target) {
    screen = target;
    repaint = true;
    last_poll = 0;
    serviced_phase = -1;
    shown_percent = 0;
    paint(BOOT_CONTROLS, 0, 0);
    boot_progress_set_handler(update);
}

void redmi2_startup_wait_wifi(void) {
    uint64_t started = time_us();
    bool timed_out = false;
    /* Run the same serialized worker used by the desktop. Polling the loading
     * screen between turns preserves power/touch handling without exposing
     * desktop controls during settings, firmware startup or entropy filling. */
    do {
        uint32_t done, total;
        redmi2_wifi_startup_progress(&done, &total);
        boot_progress_report(BOOT_WIFI, done, total);
        if (!(wifi_setup_startup_pending() || kernel_io_busy())) break;
        if (!timed_out && time_us() - started >= 90000000) {
            wifi_device_fail("Wi-Fi startup timed out");
            timed_out = true;
        }
        process_step();
    } while (true);
    /* The desktop follows: show the start complete, even if the last report
     * fell within the repaint interval. */
    shown_percent = 100;
    if (redmi2_power_screen_on()) paint(BOOT_WIFI, 100, wifi_device_status());
}

void redmi2_startup_end(void) {
    boot_progress_set_handler(0);
    screen = 0;
    /* A finger held during startup cannot become a desktop click: require a
     * fresh release (input is already suspended if the screen went off). */
    phone_input_cancel();
}
