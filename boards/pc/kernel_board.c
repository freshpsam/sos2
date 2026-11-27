#include "kernel_board.h"
#include "platform.h"
#include "display.h"
#include "page_host.h"
#include "machine.h"
#include "pc.h"

/* A PC's part of the kernel's start and main loop (kernel/main.c): the
 * screen is the GOP framebuffer SOS2's UEFI loader handed over, with the
 * 480x800 desktop at the monitor's full height in the middle of it (the
 * whole width on a portrait monitor), black bars beside it, and panics on
 * the whole monitor. PCI's devices are found at the handoff, so that the
 * kernel maps their windows; USB starts once it has, and its keyboards
 * type and its mice act as a finger, with a cursor drawn over the desktop.
 * ACPI's power button toggles standby, which only turns the desktop black:
 * GOP has no way to turn a monitor off. */
extern char __image_start[], __image_end[];

static struct framebuffer monitor;
static uint32_t desktop_left, desktop_top, desktop_width, desktop_height;

void board_handoff(struct platform_devices *d, uintptr_t dtb_address, struct framebuffer *screen) {
    (void)dtb_address;
    uintptr_t start = (uintptr_t)__image_start, end = (uintptr_t)__image_end;
    if (end <= start || !fdt_range_in_ram(d, start, end - start))
        panic("kernel static memory outside RAM");
    if (!d->has_framebuffer || !display_init(d, screen)) panic("display initialization failed");
    monitor = *screen;
    platform_set_framebuffer(&monitor);
    desktop_width = (uint32_t)((uint64_t)monitor.height * 480 / 800);
    desktop_height = monitor.height;
    if (desktop_width > monitor.width) {
        desktop_width = monitor.width;
        desktop_height = (uint32_t)((uint64_t)monitor.width * 800 / 480);
    }
    desktop_left = (monitor.width - desktop_width) / 2;
    desktop_top = (monitor.height - desktop_height) / 2;
    gfx_rect(&monitor, 0, 0, (int)monitor.width, (int)monitor.height, 0);
    screen->pixels = (unsigned char *)monitor.pixels + (size_t)desktop_top * monitor.stride +
                     (size_t)desktop_left * 4;
    screen->width = desktop_width;
    screen->height = desktop_height;
    pci_scan(d);
    xhci_probe(d);
    e1000e_probe(d);
    hda_probe(d);
}

void board_before_pages(const struct platform_devices *d) { (void)d; }

void board_pages_ready(struct platform_devices *d, struct framebuffer *screen) {
    (void)d;
    (void)screen;
    page_host_render();
    display_poll();
}

bool board_processes_ready(const struct platform_devices *d, const struct framebuffer *screen) {
    (void)d;
    (void)screen;
    usb_input_screen(monitor.width, monitor.height, desktop_left, desktop_top, desktop_width,
                     desktop_height);
    (void)xhci_start();
    acpi_power_start(d);
    return false;
}

void board_poll(void) { xhci_service(); }

enum board_power board_power_poll(void) {
    return acpi_power_pressed() ? BOARD_POWER_PRESSED : BOARD_POWER_NONE;
}

/* The cursor: an arrow whose tip is the pointer, black edged white, drawn
 * on the monitor over what it covers, which is kept to put back. */
#define CURSOR_WIDTH  12
#define CURSOR_HEIGHT 19
static const char cursor_shape[CURSOR_HEIGHT][CURSOR_WIDTH + 1] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.........X ", "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ",
    "XX   X..X   ", "X     X..X  ", "      X..X  ", "       XX   "};
static uint32_t under[CURSOR_HEIGHT][CURSOR_WIDTH];
static uint32_t cursor_x, cursor_y;
static bool cursor_shown;

static uint32_t *monitor_pixel(uint32_t x, uint32_t y) {
    return (uint32_t *)((unsigned char *)monitor.pixels + (size_t)y * monitor.stride) + x;
}

/* The cursor's pixels inside the monitor, within [x0, x1) x [y0, y1). */
static void cursor_span(uint32_t *x1, uint32_t *y1) {
    *x1 = cursor_x + CURSOR_WIDTH < monitor.width ? cursor_x + CURSOR_WIDTH : monitor.width;
    *y1 = cursor_y + CURSOR_HEIGHT < monitor.height ? cursor_y + CURSOR_HEIGHT : monitor.height;
}

static void cursor_draw(void) {
    uint32_t x1, y1;
    cursor_span(&x1, &y1);
    for (uint32_t y = cursor_y; y < y1; ++y)
        for (uint32_t x = cursor_x; x < x1; ++x) {
            char c = cursor_shape[y - cursor_y][x - cursor_x];
            if (c != ' ') *monitor_pixel(x, y) = c == 'X' ? 0x000000 : 0xffffff;
        }
}

static void cursor_keep(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x) under[y - cursor_y][x - cursor_x] = *monitor_pixel(x, y);
}

static void cursor_hide(void) {
    if (!cursor_shown) return;
    uint32_t x1, y1;
    cursor_span(&x1, &y1);
    for (uint32_t y = cursor_y; y < y1; ++y)
        for (uint32_t x = cursor_x; x < x1; ++x)
            *monitor_pixel(x, y) = under[y - cursor_y][x - cursor_x];
    cursor_shown = false;
}

static void cursor_show(uint32_t x, uint32_t y) {
    cursor_hide();
    cursor_x = x;
    cursor_y = y;
    uint32_t x1, y1;
    cursor_span(&x1, &y1);
    cursor_keep(cursor_x, cursor_y, x1, y1);
    cursor_draw();
    cursor_shown = true;
}

/* The desktop drew a region of source over part of the cursor: that part
 * is what the cursor now covers, and the cursor is drawn again. */
void pc_cursor_presented(const struct framebuffer *visible, const struct framebuffer *source,
                         unsigned x, unsigned y, unsigned width, unsigned height) {
    if (!cursor_shown || !width || !height || x >= source->width || y >= source->height) return;
    if (width > source->width - x) width = source->width - x;
    if (height > source->height - y) height = source->height - y;
    /* Its pixels on the screen, rounded as kernel/graphics.c's scaler does. */
    uint32_t left = desktop_left +
                    (uint32_t)(((uint64_t)x * visible->width + source->width - 1) / source->width);
    uint32_t right =
        desktop_left +
        (uint32_t)(((uint64_t)(x + width) * visible->width + source->width - 1) / source->width);
    uint32_t top = desktop_top + (uint32_t)(((uint64_t)y * visible->height + source->height - 1) /
                                            source->height);
    uint32_t bottom =
        desktop_top + (uint32_t)(((uint64_t)(y + height) * visible->height + source->height - 1) /
                                 source->height);
    uint32_t x1, y1;
    cursor_span(&x1, &y1);
    if (left < cursor_x) left = cursor_x;
    if (top < cursor_y) top = cursor_y;
    if (right > x1) right = x1;
    if (bottom > y1) bottom = y1;
    if (left >= right || top >= bottom) return;
    cursor_keep(left, top, right, bottom);
    cursor_draw();
}

void board_input(struct pointer_event *event) {
    struct pc_pointer p;
    for (unsigned budget = 0; budget < 16 && usb_pointer_poll(&p); ++budget) {
        cursor_show((uint32_t)p.x, (uint32_t)p.y);
        *event = (struct pointer_event){(uint32_t)p.x - desktop_left, (uint32_t)p.y - desktop_top,
                                        p.down, false};
        page_host_input(event);
        if (p.wheel) page_host_wheel(event, 0, p.wheel);
    }
    struct user_input_event key;
    for (unsigned budget = 0; budget < 32 && usb_keyboard_poll(&key); ++budget)
        page_host_keyboard(&key);
}

void board_after_frame(void) { }

void board_screen_standby(bool on, struct pointer_event *cancel) {
    if (!on) return;
    *cancel = (struct pointer_event){.cancelled = true};
    cursor_hide();
    display_blank();
}

void board_system_standby(bool on) { (void)on; }

/* Input while the screen is off acts on nothing. */
void board_standby_step(struct pointer_event *scratch) {
    (void)scratch;
    struct pc_pointer p;
    struct user_input_event key;
    for (unsigned budget = 0; budget < 16 && usb_pointer_poll(&p); ++budget) { }
    for (unsigned budget = 0; budget < 32 && usb_keyboard_poll(&key); ++budget) { }
}

void board_standby_sleep(void) { cpu_sleep_us(BOARD_STANDBY_POLL_US); }
