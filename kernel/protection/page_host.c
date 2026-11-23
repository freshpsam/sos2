#include "page_host.h"
#include "platform.h"
#include "screen_keyboard.h"
#include "process.h"
#include "desktop_wifi.h"
#include "system_clock.h"
#include "battery.h"
#include "display.h"
#include "pixel_frame.h"
#include "apps.h"
#include "haptics.h"

/* The shell owns focus and bounded event queues as well as presentation. */
#define EVENT_COUNT 128u

struct page {
    struct page *next;
    unsigned slot, head, count, viewport;
    bool ready, keyboard_wanted;
    bool back_key; /* SYS_BACK_KEY: Back arrives as key 158 */
    /* SYS_HAPTIC_TAP: presses sent here that may each still get a buzz, until
     * tap_until, 250 ms after the latest. */
    unsigned taps;
    uint64_t tap_until;
    /* GPU frames (USER_GPU_PRESENT): the compositor draws the page from its
     * GPU snapshot, and pixels below are stale until a whole CPU frame. */
    bool gpu_frames;
    /* The app this slot runs (include/apps.h), or NULL for a web page. Apps
     * start from the launcher and stay out of the WEB PAGES list. */
    const struct app *app;
    struct user_input_event events[EVENT_COUNT];
    struct user_browser_state browser;
    struct user_browser_command command;
    uint32_t pixels[WEB_WIDTH * WEB_HEIGHT];
};

#define PAGE_HOST_PAGES ((sizeof(struct page) + PAGE_SIZE - 1) / PAGE_SIZE)
static struct page *pages;

static struct page *page(unsigned slot) {
    for (struct page *p = pages; p; p = p->next)
        if (p->slot == slot) return p;
    return 0;
}

static unsigned page_count(void) {
    unsigned n = 0;
    for (struct page *p = pages; p; p = p->next)
        if (!p->app) ++n;
    return n;
}

static unsigned page_offset;

enum {
    LIST_ROW = 56,
    ACTION_APP = 500,
    ACTION_PAGE = 1000,
    STATUS_TOP = WEB_TOP + 16,
    APP_COLUMNS = 5,
    APP_BUTTON = 80,
    APP_GAP = 12,
    TAP_WINDOW_US = 250000,
    TAP_CREDITS = 4,
    VOLUME_SHOWN_US = 2000000
};

static unsigned list_top(void), list_bottom(void), list_rows(void);
static int list_drag_y;
static unsigned list_drag_offset;
static bool list_dragging;
static void scroll_pages(int offset);
static void page_list_render(struct framebuffer *, unsigned, unsigned);

static struct framebuffer *display;
static bool dirty, pressed;

static void scroll_pages(int offset) {
    unsigned count = page_count(), last = count > list_rows() ? count - list_rows() : 0;
    unsigned next = offset < 0 ? 0 : (unsigned)offset > last ? last : (unsigned)offset;
    if (next != page_offset && list_rows()) {
        page_offset = next;
        page_host_redraw_region(0, list_top(), 480, list_bottom() - list_top(), page_list_render);
    }
}

static bool address_damage, frame_presented;
/* Page view that the composition last showed in full; drivers may cache it. */
static uint32_t composed_view = UINT32_MAX;
static bool screen_active;

static struct {
    unsigned left, top, right, bottom;
} web_damage;

/* Where the display scales on the CPU (display_bands), a page's frame is drawn
 * a band of rows a turn: scaling a video frame took the Nexus 5 1.4 ms, all of
 * it holding CPU 0's loop and every system call. A pass copies the damaged
 * rows into the composition at once, so that a frame the page presents
 * meanwhile waits for the next pass rather than mixing with this one, scales
 * WEB_BAND_PIXELS of them a turn, and shows the region once the last band is
 * in. Rows from next to bottom are left (none when they are equal). */
static struct {
    unsigned left, width, top, next, bottom;
} web_pass;

enum { WEB_BAND_PIXELS = WEB_WIDTH * 32 };

static struct widget_damage {
    unsigned x, y, width, height;
    void (*paint)(struct framebuffer *, unsigned, unsigned);
} widget_damage[8];

static unsigned widget_count;
static uint64_t keyboard_damage;

/* device_message: why the touch screen, then the USB link, is unavailable. */
enum { DEVICE_TOUCH, DEVICE_USB, DEVICES };

static char entropy_message[73], device_message[DEVICES][73];
static bool entropy_ready, storage_ready, storage_session;
static unsigned selected, current_modifiers;
static int toolbar_press = -1, focused = -1, pointer_owner = -1, soft_owner = -1, key_owners[256];
static struct screen_keyboard keyboard;
/* CPU-only retained composition. Device queues take their own snapshots; no
 * DMA engine sees this cached surface or the CPU-private section directly. */
static uint32_t composition_pixels[WEB_WIDTH * 800]
    __attribute__((aligned(4096), section(".cpu_private")));
static struct framebuffer composition = {composition_pixels, WEB_WIDTH, 800, WEB_WIDTH * 4,
                                         PIXEL_XRGB8888};
static bool panel, address_edit, address_all;
static char address[BROWSER_URL_MAX + 1];
static unsigned address_length, address_cursor, address_offset;
static int panel_press = -1;
/* The app a launcher press is on: an install or removal while the finger is
 * down can give its index to another app. */
static const struct app *pressed_app;
static const char *browser_message;
static void soft_cancel(void);
static void update_keyboard(void);
static void browser_panel(bool open);
static void browser_action(unsigned action);
static void address_key(const struct user_input_event *, char text);
static void status_tick(void);
static void status_render(struct framebuffer *, unsigned, unsigned);
static uint64_t status_minute = UINT64_MAX;
static struct battery_view status_battery;
/* When status_tick next reads the clock and the battery: the clock shows
 * minutes, and both change slowly, so not every turn (that read the timer and
 * divided its time by 60 s in 64 bits). */
static uint64_t status_next;

enum { STATUS_PERIOD_US = 100000 };

/* The volume shown until volume_until (0: the clock is). */
static unsigned volume_level, volume_steps;
static bool volume_headphones;
static uint64_t volume_until;

enum {
    ACTION_ADDRESS = 100,
    ACTION_OPEN,
    ACTION_CLOSE,
    ACTION_WIFI,
    ACTION_UPDATES,
    ACTION_HOME,
    ACTION_BACK
};

/* The top bar's buttons: CLOSE over a page and, on the Galaxy Tab A7 Lite
 * and the Nexus 5, which have no Home or Back key, HOME and BACK over a page
 * or the Wi-Fi screen, BACK on the right as on Samsung's navigation bar. */
#ifdef SOS2_NAVIGATION_BUTTONS
enum { NAVIGATION_BUTTONS = 1 };
#else
enum { NAVIGATION_BUTTONS = 0 };
#endif
/* The tablet's three run from the left margin to the clock, 6 pixels apart
 * like their margins above and below; the phone's lone CLOSE is narrower. */
enum {
    HEADER_BUTTON = NAVIGATION_BUTTONS ? 104 : 84,
    HEADER_GAP = 6,
    CLOSE_X = NAVIGATION_BUTTONS ? 16 : 12,
    HOME_X = CLOSE_X + HEADER_BUTTON + HEADER_GAP,
    BACK_X = HOME_X + HEADER_BUTTON + HEADER_GAP
};

static bool navigation_buttons(void) {
    return NAVIGATION_BUTTONS && (!panel || desktop_wifi_visible());
}

static int header_hit(unsigned x, unsigned y) {
    if (y < 6 || y >= 42) return -1;
    if (!panel && x >= CLOSE_X && x < CLOSE_X + HEADER_BUTTON) return ACTION_CLOSE;
    if (!navigation_buttons()) return -1;
    if (x >= HOME_X && x < HOME_X + HEADER_BUTTON) return ACTION_HOME;
    if (x >= BACK_X && x < BACK_X + HEADER_BUTTON) return ACTION_BACK;
    return -1;
}

/* Desktop, top to bottom: status lines, the WI-FI and UPDATES buttons, the
 * WEB PAGES label, the address row, an Open error, the page list and, at the
 * bottom, the APPS launcher. The address row moves only with the status
 * lines, which settle at startup, never while typing. */
static unsigned settings_top(void) {
    unsigned lines = 1;
    if (!entropy_ready) lines += 1 + (entropy_message[0] != 0);
    for (unsigned i = 0; i < DEVICES; ++i) lines += device_message[i][0] != 0;
    return STATUS_TOP + lines * 18 + 8;
}

static unsigned address_top(void) { return settings_top() + 92; }

static unsigned list_top(void) { return address_top() + 64 + (browser_message ? 18 : 0); }

/* Room for as many apps as the launcher lists (APP_LIMIT) however many there
 * are, in rows of APP_COLUMNS square buttons ending 16 pixels above the
 * bottom; none without apps. */
static unsigned launcher_rows(void) {
    return apps_count() ? (APP_LIMIT + APP_COLUMNS - 1) / APP_COLUMNS : 0;
}

static unsigned launcher_top(void) {
    unsigned rows = launcher_rows();
    return rows ? 800 - 16 - rows * APP_BUTTON - (rows - 1) * APP_GAP : 800;
}

static unsigned list_bottom(void) { return launcher_rows() ? launcher_top() - 32 : 788; }

static unsigned list_rows(void) {
    unsigned top = list_top(), bottom = list_bottom();
    return bottom > top ? (bottom - top) / LIST_ROW : 0;
}

static int launcher_hit(unsigned x, unsigned y) {
    unsigned top = launcher_top(), pitch = APP_BUTTON + APP_GAP;
    if (x < 16 || y < top || (x - 16) % pitch >= APP_BUTTON || (y - top) % pitch >= APP_BUTTON)
        return -1;
    unsigned column = (x - 16) / pitch, index = (y - top) / pitch * APP_COLUMNS + column;
    return column < APP_COLUMNS && index < apps_count() ? (int)index : -1;
}

static void damage_key(int id) {
    if (id >= 0 && id < 64) keyboard_damage |= UINT64_C(1) << id;
}

static struct pointer_event logical_pointer(const struct pointer_event *point) {
    struct pointer_event result = *point;
    if (display) {
        result.x = point->x >= display->width ? WEB_WIDTH
                                              : (uint64_t)point->x * WEB_WIDTH / display->width;
        result.y = point->y >= display->height ? 800 : (uint64_t)point->y * 800 / display->height;
    }
    return result;
}

static void forget_keys(unsigned slot) {
    for (unsigned i = 0; i < 256; ++i)
        if (key_owners[i] == (int)slot) key_owners[i] = -1;
}

static struct user_input_event focus_event(unsigned slot, unsigned type) {
    return (struct user_input_event){.type = type,
                                     .flags = focused == (int)slot ? INPUT_FOCUSED : 0,
                                     .modifiers = focused == (int)slot ? current_modifiers : 0,
                                     .x = WEB_WIDTH,
                                     .y = page(slot)->viewport};
}

static void enqueue(unsigned slot, struct user_input_event event) {
    struct page *p = page(slot);
    if (!p) return;
    if (p->count == EVENT_COUNT) {
        /* Lost releases cannot leave a drag, modifier or key held. Drop stale
         * input and explicitly reset WebKit instead of synthesizing a click. */
        p->head = 0;
        p->count = 1;
        forget_keys(slot);
        if (pointer_owner == (int)slot) pointer_owner = -1;
        if (focused == (int)slot) {
            screen_keyboard_cancel(&keyboard, 0, 0);
            dirty = true;
        }
        p->events[0] = focus_event(slot, INPUT_RESET);
        return;
    }
    /* Pure motion can replace pure motion, never a button/key transition. */
    if (p->count && event.type == INPUT_POINTER && !event.code && !(event.flags & INPUT_CANCEL)) {
        unsigned at = (p->head + p->count - 1) % EVENT_COUNT;
        if (p->events[at].type == INPUT_POINTER && !p->events[at].code &&
            p->events[at].flags == event.flags && p->events[at].modifiers == event.modifiers) {
            p->events[at] = event;
            return;
        }
    }
    p->events[(p->head + p->count++) % EVENT_COUNT] = event;
}

static void set_focus(int next) {
    if (next >= 0 && !page((unsigned)next)) next = -1;
    if (focused == next) return;
    soft_cancel();
    keyboard.shift = false;
    pointer_owner = -1;
    int old = focused;
    focused = next;
    if (old >= 0) {
        page(old)->count = page(old)->head = 0;
        forget_keys(old);
        enqueue(old, focus_event(old, INPUT_FOCUS));
    }
    if (next >= 0) {
        page(next)->count = page(next)->head = 0;
        forget_keys(next);
        enqueue(next, focus_event(next, INPUT_FOCUS));
    }
    update_keyboard();
}

static void soft_emit(void *context, const struct user_input_event *key, char text) {
    (void)context;
    if (key->flags & INPUT_DOWN) {
        if (!(key->flags & INPUT_REPEAT))
            soft_owner = desktop_wifi_editing() ? -3 : address_edit ? -2 : focused;
        if (soft_owner == -3) {
            if (desktop_wifi_visible()) desktop_wifi_key(key, text);
            update_keyboard();
            return;
        }
        if (soft_owner == -2) {
            if (address_edit) address_key(key, text);
            return;
        }
        if (soft_owner != focused) return;
    } else {
        int owner = soft_owner;
        soft_owner = -1;
        /* Enter can open a new renderer while dispatching this same soft key.
         * Its release still belongs to the address editor, never the new page. */
        if (owner < 0 || owner != focused) return;
    }
    if (focused < 0) return;
    struct user_input_event event = *key;
    event.modifiers |= current_modifiers;
    enqueue(focused, event);
}

static void soft_cancel(void) {
    int before = keyboard.pressed;
    screen_keyboard_cancel(&keyboard, soft_emit, 0);
    if (before != keyboard.pressed) damage_key(before);
}

static void update_keyboard(void) {
    bool visible = desktop_wifi_editing() || address_edit ||
                   (!panel && focused == (int)selected && focused >= 0 && page(selected) &&
                    page(selected)->keyboard_wanted);
    if (visible != keyboard.visible) {
        screen_keyboard_show(&keyboard, visible, soft_emit, 0);
        dirty = true;
    }
    for (struct page *p = pages; p; p = p->next) {
        unsigned slot = p->slot;
        /* The desktop's own editors use the keyboard over the panel; never
         * resize the hidden selected page for them. */
        unsigned height = visible && !panel && selected == slot
                              ? screen_keyboard_top(&keyboard) - WEB_TOP
                              : WEB_HEIGHT;
        if (p->viewport != height) {
            p->viewport = height;
            enqueue(slot,
                    (struct user_input_event){.type = INPUT_VIEWPORT, .x = WEB_WIDTH, .y = height});
        }
    }
}

int32_t page_host_input_method(unsigned slot, unsigned enabled) {
    struct page *p = page(slot);
    if (enabled > 1) return USER_EINVAL;
    if (!display || !p) return USER_ENODEV;
    p->keyboard_wanted = enabled != 0;
    update_keyboard();
    return 0;
}

int32_t page_host_back_key(unsigned slot, unsigned enabled) {
    struct page *p = page(slot);
    if (enabled > 1) return USER_EINVAL;
    if (!display || !p) return USER_ENODEV;
    p->back_key = enabled != 0;
    return 0;
}

int32_t page_host_haptic_tap(unsigned slot) {
    struct page *p = page(slot);
    if (!display || !p) return USER_ENODEV;
    if (!screen_active || !p->taps || time_us() >= p->tap_until) return USER_EAGAIN;
    --p->taps;
    haptics_tap();
    return 0;
}

static bool soft_input(const struct pointer_event *event) {
    struct screen_keyboard before = keyboard;
    bool consumed = screen_keyboard_handle(&keyboard, event, time_us(), soft_emit, 0);
    /* A finger landing on a key; a held Backspace repeats without one. */
    if (keyboard.pressed >= 0 && keyboard.pressed != before.pressed) haptics_tap();
    if (before.visible && !keyboard.visible) {
        address_edit = false;
        desktop_wifi_hide_keyboard();
        if (page(selected)) page(selected)->keyboard_wanted = false;
        update_keyboard();
    }
    if (before.visible != keyboard.visible) dirty = true;
    if (before.shift != keyboard.shift || before.symbols != keyboard.symbols)
        keyboard_damage = UINT64_MAX;
    else if (before.pressed != keyboard.pressed) {
        damage_key(before.pressed);
        damage_key(keyboard.pressed);
    }
    return consumed;
}

static void cancel_pointer(unsigned slot) {
    /* Only a gesture this page owns needs cancelling. Unowned cancels (e.g.
     * hovering the header) would fill the queue and reset click counting. */
    if (pointer_owner != (int)slot) return;
    pointer_owner = -1;
    enqueue(slot, (struct user_input_event){.type = INPUT_POINTER,
                                            .flags = INPUT_CANCEL,
                                            .modifiers = current_modifiers});
}

void page_host_init(struct framebuffer *screen) {
    while (pages) {
        struct page *next = pages->next;
        vm_cpu_free(pages, PAGE_HOST_PAGES);
        pages = next;
    }
    display = screen;
    selected = UINT32_MAX;
    current_modifiers = 0;
    dirty = true;
    pressed = false;
    address_damage = frame_presented = false;
    keyboard_damage = 0;
    widget_count = 0;
    composed_view = UINT32_MAX;
    screen_active = true;
    web_damage.right = web_damage.bottom = 0;
    web_pass.next = web_pass.bottom = 0;
    entropy_message[0] = device_message[DEVICE_TOUCH][0] = device_message[DEVICE_USB][0] = 0;
    address[0] = 0;
    address_length = address_cursor = address_offset = page_offset = 0;
    list_dragging = false;
    toolbar_press = pointer_owner = soft_owner = focused = -1;
    memset(key_owners, 0xff, sizeof(key_owners));
    screen_keyboard_init(&keyboard, WEB_WIDTH, 800);
    panel = true;
    address_edit = address_all = false;
    panel_press = -1;
    browser_message = 0;
    status_minute = UINT64_MAX;
    status_battery = (struct battery_view){0};
    status_next = 0;
    desktop_wifi_close();
    desktop_wifi_network_status(0);
}

static bool page_host_select(unsigned slot) {
    if (!page(slot)) return false;
    set_focus(-1);
    selected = slot;
    browser_panel(false);
    return true;
}

static int page_hit(unsigned y) {
    if (y < list_top() || y >= list_top() + list_rows() * LIST_ROW) return -1;
    unsigned index = page_offset + (y - list_top()) / LIST_ROW;
    for (struct page *p = pages; p; p = p->next)
        if (!p->app && !index--) return (int)p->slot;
    return -1;
}

bool page_host_input(const struct pointer_event *event) {
    if (!display || !event) return false;
    struct pointer_event logical = logical_pointer(event);
    event = &logical;
    bool release = pressed && !event->down && !event->cancelled,
         press = !pressed && event->down && !event->cancelled;
    int toolbar = header_hit(event->x, event->y);
    if (press) {
        toolbar_press = toolbar;
        /* CLOSE takes the page's focus at once and HOME on release; BACK
         * leaves it with the page, as the Back key does. */
        if (toolbar == ACTION_CLOSE) set_focus(-1);
        if (toolbar >= 0) haptics_tap();
    }
    pressed = event->down && !event->cancelled;
    if (soft_input(event)) {
        if (pointer_owner >= 0) cancel_pointer((unsigned)pointer_owner);
        return true;
    }
    if (event->cancelled) {
        desktop_wifi_pointer(event, false, false);
        toolbar_press = panel_press = -1;
        list_dragging = false;
        cancel_pointer(selected);
        return true;
    }
    if (panel) {
        if (desktop_wifi_visible()) {
            if (toolbar_press >= 0) { /* begun on HOME or BACK: stays in the top bar */
                if (release && toolbar == toolbar_press) browser_action((unsigned)toolbar);
                if (!event->down) toolbar_press = -1;
                return true;
            }
            desktop_wifi_pointer(event, press, release);
            update_keyboard();
            return true;
        }
        int hit = -1;
        if (event->x >= 16 && event->x < 464) {
            if (event->y >= address_top() && event->y < address_top() + 48)
                hit = event->x < 352 ? ACTION_ADDRESS : event->x >= 364 ? ACTION_OPEN : -1;
            else if (event->y >= settings_top() && event->y < settings_top() + 44)
                hit = event->x < 232 ? ACTION_WIFI : event->x >= 248 ? ACTION_UPDATES : -1;
            else if (event->y >= launcher_top()) {
                int app = launcher_hit(event->x, event->y);
                if (app >= 0) hit = ACTION_APP + app;
            } else {
                int slot = page_hit(event->y);
                if (slot >= 0) hit = ACTION_PAGE + slot;
            }
        }
        if (press) {
            panel_press = hit;
            pressed_app =
                hit >= ACTION_APP && hit < ACTION_PAGE ? app_at((unsigned)(hit - ACTION_APP)) : 0;
            if (hit >= 0 && hit != ACTION_ADDRESS) haptics_tap(); /* a button, not the field */
            list_dragging = event->y >= list_top() && event->y < list_bottom();
            list_drag_y = (int)event->y;
            list_drag_offset = page_offset;
        }
        if (list_dragging && (event->down || release)) {
            int dy = list_drag_y - (int)event->y;
            if (dy > 12 || dy < -12) {
                panel_press = -1;
                scroll_pages((int)list_drag_offset + dy / LIST_ROW);
            }
        }
        if (event->down && hit != panel_press) panel_press = -1;
        if (hit >= ACTION_APP && hit < ACTION_PAGE &&
            app_at((unsigned)(hit - ACTION_APP)) != pressed_app)
            panel_press = -1;
        if (release && hit >= 0 && hit == panel_press) browser_action((unsigned)hit);
        if (!event->down) {
            panel_press = -1;
            list_dragging = false;
        }
        return true;
    }
    struct page *p = page(selected);
    if (!p) {
        browser_panel(true);
        return true;
    }
    if (event->x >= WEB_WIDTH || event->y < WEB_TOP || event->y >= WEB_TOP + p->viewport) {
        cancel_pointer(selected);
        if (release && toolbar >= 0 && toolbar == toolbar_press) browser_action((unsigned)toolbar);
        if (!event->down) toolbar_press = -1;
        return true;
    }
    if (release) toolbar_press = -1;
    if (press) {
        set_focus(selected);
        pointer_owner = (int)selected;
        uint64_t now = time_us();
        if (now >= p->tap_until) p->taps = 0;
        if (p->taps < TAP_CREDITS) ++p->taps;
        p->tap_until = now + TAP_WINDOW_US;
    }
    /* A drag cannot migrate out of trusted controls or another page. */
    if ((event->down || release) && pointer_owner != (int)selected) return true;
    enqueue(selected, (struct user_input_event){.type = INPUT_POINTER,
                                                .flags = event->down ? INPUT_DOWN : 0,
                                                .modifiers = current_modifiers,
                                                .code = press     ? 1u
                                                        : release ? 2u
                                                                  : 0u,
                                                .x = event->x,
                                                .y = event->y - WEB_TOP});
    if (release) pointer_owner = -1;
    return true;
}

void page_host_wheel(const struct pointer_event *point, int32_t dx, int32_t dy) {
    if (!display || !point) return;
    if (panel) {
        if (!desktop_wifi_visible() && !address_edit && dy)
            scroll_pages((int)page_offset + (dy < 0 ? 1 : -1));
        return;
    }
    if (!page(selected)) return;
    struct pointer_event logical = logical_pointer(point);
    point = &logical;
    if (point->cancelled || (!dx && !dy) || point->x >= WEB_WIDTH || point->y < WEB_TOP ||
        point->y >= WEB_TOP + page(selected)->viewport)
        return;
    enqueue(selected, (struct user_input_event){.type = INPUT_WHEEL,
                                                .modifiers = current_modifiers,
                                                .x = point->x,
                                                .y = point->y - WEB_TOP,
                                                .delta_x = dx,
                                                .delta_y = dy});
}

void page_host_back(void) {
    if (!display) return;
    if (desktop_wifi_visible()) {
        desktop_wifi_back();
        update_keyboard();
        return;
    }
    if (panel) return;
    struct page *p = page(selected);
    if (p && p->back_key) {
        struct user_input_event key = {
            .type = INPUT_KEY, .flags = INPUT_DOWN, .modifiers = current_modifiers, .code = 158};
        enqueue(selected, key);
        key.flags = 0;
        enqueue(selected, key);
        return;
    }
    if (!p || p->app || p->command.action || !(p->browser.flags & BROWSER_CAN_GO_BACK)) return;
    p->command = (struct user_browser_command){.action = BROWSER_BACK};
    p->keyboard_wanted = false;
    update_keyboard();
    set_focus((int)selected);
}

void page_host_home(void) {
    if (display) browser_panel(true);
}

void page_host_keyboard(const struct user_input_event *event) {
    if (!display || !event) return;
    if (event->type == INPUT_RESET) {
        soft_cancel();
        pointer_owner = -1;
        dirty = true;
        current_modifiers = 0;
        memset(key_owners, 0xff, sizeof(key_owners));
        for (struct page *p = pages; p; p = p->next) {
            p->count = p->head = 0;
            enqueue(p->slot, focus_event(p->slot, INPUT_RESET));
        }
        return;
    }
    if (event->type != INPUT_KEY || event->code >= 256) return;
    current_modifiers = event->modifiers;
    /* Shortcuts take only a fresh press, which then belongs to no page: its
     * repeats and release go nowhere. Keys pressed before a modifier keep
     * their owner, so a page still sees the release of a key it saw go down. */
    bool press = (event->flags & (INPUT_DOWN | INPUT_REPEAT)) == INPUT_DOWN,
         ctrl = event->modifiers & INPUT_CTRL;
    /* QEMU has no phone Home or Back key: a keyboard's Home page and Back
     * keys, or Alt+Home and Alt+Left. */
    bool alt = event->modifiers & INPUT_ALT,
         back = press && (event->code == 158 || (event->code == 105 && alt));
    if (press && (event->code == 172 || (event->code == 102 && alt))) {
        key_owners[event->code] = -1;
        page_host_home();
        return;
    }
    if (desktop_wifi_visible()) {
        if (back) {
            key_owners[event->code] = -1;
            page_host_back();
            return;
        }
        desktop_wifi_key(event, 0);
        update_keyboard();
        return;
    }
    if (press && ctrl && event->code == 38) {
        key_owners[event->code] = -1;
        browser_panel(true);
        browser_action(ACTION_ADDRESS);
        return;
    }
    if (press && ctrl && event->code == 17) {
        key_owners[event->code] = -1;
        browser_action(ACTION_CLOSE);
        return;
    }
    if (panel) {
        address_key(event, 0);
        return;
    }
    if (back) {
        key_owners[event->code] = -1;
        page_host_back();
        return;
    }
    if (press && ctrl && event->code == 15) {
        key_owners[event->code] = -1;
        struct page *p = page(selected);
        if (p) page_host_select(p->next ? p->next->slot : pages->slot);
        return;
    }
    if (event->flags & INPUT_DOWN) {
        if (!(event->flags & INPUT_REPEAT)) key_owners[event->code] = focused;
        if (focused >= 0 && key_owners[event->code] == focused) enqueue(focused, *event);
    } else {
        if (focused >= 0 && key_owners[event->code] == focused) enqueue(focused, *event);
        key_owners[event->code] = -1;
    }
}

int32_t page_host_present(unsigned slot, const struct vm_space *space, user_ptr address,
                          uint32_t bytes) {
    if (bytes != WEB_WIDTH * WEB_HEIGHT * 4) return USER_EINVAL;
    struct user_present_damage damage = {address, 0, 0, WEB_WIDTH, WEB_HEIGHT};
    return page_host_present_damage(slot, space, &damage);
}

unsigned page_host_surface_state(unsigned slot) {
    return (display && screen_active && page(slot) && slot == selected && !panel
                ? USER_SURFACE_VISIBLE
                : 0) |
           (display_gpu_frames() ? USER_SURFACE_GPU_FRAMES : 0);
}

void page_host_screen_active(bool active) {
    screen_active = active;
    if (active) dirty = true;
}

int32_t page_host_present_damage(unsigned slot, const struct vm_space *space,
                                 const struct user_present_damage *d) {
    if (!d) return USER_EINVAL;
    struct user_present_frame frame = {
        .pixels = d->pixels, .count = 1, .damage = {{d->x, d->y, d->width, d->height}}};
    return page_host_present_frame(slot, space, &frame);
}

static void page_damage(struct user_pixel_rect r) {
    if (!web_damage.right) web_damage.left = r.x, web_damage.top = r.y;
    if (r.x < web_damage.left) web_damage.left = r.x;
    if (r.y < web_damage.top) web_damage.top = r.y;
    if (r.x + r.width > web_damage.right) web_damage.right = r.x + r.width;
    if (r.y + r.height > web_damage.bottom) web_damage.bottom = r.y + r.height;
}

int32_t page_host_gpu_frame_check(unsigned slot, const struct user_pixel_rect *rects,
                                  unsigned count) {
    struct page *p = page(slot);
    if (!display || !p || !count || count > USER_FRAME_RECTS) return USER_EINVAL;
    for (unsigned i = 0; i < count; ++i)
        if (!pixel_rect_valid(rects[i])) return USER_EINVAL;
    if (!p->ready && (count != 1 || rects[0].x || rects[0].y || rects[0].width != WEB_WIDTH ||
                      rects[0].height != WEB_HEIGHT))
        return USER_EINVAL;
    return 0;
}

void page_host_gpu_frame(unsigned slot, const struct user_pixel_rect *rects, unsigned count) {
    struct page *p = page(slot);
    if (!p) return;
    p->ready = true;
    p->gpu_frames = true;
    for (unsigned i = 0; i < count; ++i) {
        struct user_pixel_rect r = rects[i];
        display_page_changed(slot, r.x, r.y, r.width, r.height);
        if (slot == selected && !panel) page_damage(r);
    }
}

int32_t page_host_present_frame(unsigned slot, const struct vm_space *space,
                                const struct user_present_frame *f) {
    struct page *p = page(slot);
    if (!display || !p || !f || f->count > USER_FRAME_RECTS) return USER_EINVAL;
    const struct user_pixel_scroll *s = &f->scroll;
    bool scrolling = s->rect.width != 0;
    if (scrolling ? !pixel_scroll_valid(s)
                  : (s->rect.x || s->rect.y || s->rect.height || s->dx || s->dy))
        return USER_EINVAL;
    for (unsigned i = 0; i < f->count; ++i)
        if (!pixel_rect_valid(f->damage[i])) return USER_EINVAL;
    if ((!p->ready || p->gpu_frames) &&
        (scrolling || f->count != 1 || f->damage[0].x || f->damage[0].y ||
         f->damage[0].width != WEB_WIDTH || f->damage[0].height != WEB_HEIGHT))
        return USER_EINVAL;
    /* All rows of all patches must be readable before even the retained scroll
     * is applied. VM operations and this copy are serialized on the boot CPU. */
    for (unsigned i = 0; i < f->count; ++i) {
        struct user_pixel_rect r = f->damage[i];
        uint32_t end = ((r.y + r.height - 1) * WEB_WIDTH + r.x + r.width) * 4;
        if (f->pixels > (user_ptr)-1 - (end - 1)) return USER_EFAULT;
        for (unsigned y = r.y; y < r.y + r.height; ++y)
            if (!vm_accessible(space, f->pixels + (y * WEB_WIDTH + r.x) * 4, r.width * 4, VM_READ))
                return USER_EFAULT;
    }
    if (scrolling) {
        struct user_pixel_rect src, dst;
        pixel_scroll_boxes(s, &src, &dst);
        if (dst.width == WEB_WIDTH) {
            memmove(p->pixels + dst.y * WEB_WIDTH, p->pixels + src.y * WEB_WIDTH,
                    (size_t)dst.height * WEB_WIDTH * sizeof(*p->pixels));
        } else
            for (unsigned i = 0; i < dst.height; ++i) {
                unsigned row = s->dy > 0 ? dst.height - 1 - i : i;
                uint32_t *out = p->pixels + (dst.y + row) * WEB_WIDTH + dst.x;
                const uint32_t *in = p->pixels + (src.y + row) * WEB_WIDTH + src.x;
                memmove(out, in, dst.width * sizeof(*out));
            }
    }
    for (unsigned i = 0; i < f->count; ++i) {
        struct user_pixel_rect r = f->damage[i];
        for (unsigned y = r.y; y < r.y + r.height; ++y)
            if (!vm_copy_from_user(space, p->pixels + y * WEB_WIDTH + r.x,
                                   f->pixels + (y * WEB_WIDTH + r.x) * 4, r.width * 4))
                return USER_EFAULT;
    }
    p->ready = true;
    if (p->gpu_frames) {
        p->gpu_frames = false;
        display_page_cpu_frames(slot);
    }
    if (scrolling) {
        display_page_scroll(slot, s);
        if (slot == selected && !panel) page_damage(s->rect);
    }
    for (unsigned i = 0; i < f->count; ++i) {
        struct user_pixel_rect r = f->damage[i];
        display_page_changed(slot, r.x, r.y, r.width, r.height);
        if (slot == selected && !panel) page_damage(r);
    }
    return 0;
}

bool page_host_next(unsigned slot, struct user_input_event *event) {
    struct page *p = page(slot);
    if (!p || !event || !p->count) return false;
    *event = p->events[p->head];
    p->head = (p->head + 1) % EVENT_COUNT;
    --p->count;
    return true;
}

void page_host_closed(unsigned slot) {
    struct page **link = &pages;
    while (*link && (*link)->slot != slot) link = &(*link)->next;
    if (!*link) return;
    if (focused == (int)slot) set_focus(-1);
    struct page *p = *link;
    *link = p->next;
    display_page_closed(slot);
    if (composed_view == slot) composed_view = UINT32_MAX;
    forget_keys(slot);
    if (pointer_owner == (int)slot) pointer_owner = -1;
    if (selected == slot) {
        selected = UINT32_MAX;
        browser_panel(true);
    }
    vm_cpu_free(p, PAGE_HOST_PAGES);
    scroll_pages((int)page_offset);
    dirty = true;
    update_keyboard();
}

bool page_host_opened(unsigned slot, const struct app *app) {
    struct page *p = page(slot);
    if (!p) {
        /* These retained pixels are CPU-only. Display drivers copy their own
         * immutable DMA snapshots before submitting work. */
        p = vm_cpu_alloc(PAGE_HOST_PAGES);
        if (!p) return false;
        p->slot = slot;
        p->viewport = WEB_HEIGHT;
        struct page **link = &pages;
        while (*link && (*link)->slot < slot) link = &(*link)->next;
        p->next = *link;
        *link = p;
    }
    display_page_closed(slot); /* A new process lifetime must not reuse old pixels. */
    if (composed_view == slot) composed_view = UINT32_MAX;
    p->app = app;
    p->ready = false;
    p->head = p->count = 0;
    p->keyboard_wanted = p->back_key = false;
    p->taps = 0;
    p->tap_until = 0;
    p->browser.flags = 0;
    enqueue(slot, focus_event(slot, INPUT_FOCUS));
    update_keyboard();
    dirty = true;
    return true;
}

void page_host_start_failed(unsigned slot) {
    const struct app *app = page(slot) ? page(slot)->app : 0;
    page_host_closed(slot);
    browser_message = !app          ? "UNABLE TO START PAGE / CHECK AVAILABLE MEMORY"
                      : app->system ? "UNABLE TO START UPDATES / CHECK AVAILABLE MEMORY"
                                    : "UNABLE TO START APP / CHECK AVAILABLE MEMORY";
    dirty = true;
}

void page_host_redraw(void) { dirty = true; }

void page_host_redraw_region(unsigned x, unsigned y, unsigned width, unsigned height,
                             void (*paint)(struct framebuffer *, unsigned, unsigned)) {
    if (!paint || !width || !height || x >= WEB_WIDTH || y >= 800 || width > WEB_WIDTH - x ||
        height > 800 - y)
        return;
    for (unsigned i = 0; i < widget_count; ++i) {
        const struct widget_damage *d = &widget_damage[i];
        if (d->paint == paint && d->x == x && d->y == y && d->width == width && d->height == height)
            return;
    }
    if (widget_count == sizeof(widget_damage) / sizeof(widget_damage[0])) {
        dirty = true;
        return;
    }
    widget_damage[widget_count++] = (struct widget_damage){x, y, width, height, paint};
}

void page_host_network_status(const char *message) { desktop_wifi_network_status(message); }

void page_host_device_status(bool entropy, bool storage, bool session) {
    entropy_ready = entropy;
    storage_ready = storage;
    storage_session = session;
    dirty = true;
}

void page_host_clock_status(void) {
    status_next = 0;
    status_tick();
}

static void device_status(unsigned device, const char *prefix, const char *error) {
    char text[sizeof(device_message[0])] = {0};
    if (error) {
        unsigned n = 0;
        for (; prefix[n]; ++n) text[n] = prefix[n];
        for (const char *e = error; *e && n < sizeof(text) - 1; ++e) text[n++] = *e;
    }
    if (strcmp(text, device_message[device])) {
        memcpy(device_message[device], text, sizeof(text));
        dirty = true;
    }
}

void page_host_touch_status(const char *error) {
    device_status(DEVICE_TOUCH, "TOUCH UNAVAILABLE: ", error);
}

void page_host_usb_status(const char *error) {
    device_status(DEVICE_USB, "USB UNAVAILABLE: ", error);
}

void page_host_entropy_status(bool ready, const char *error) {
    char text[sizeof(entropy_message)] = {0};
    unsigned n = 0;
    if (!ready && error)
        while (n < sizeof(text) - 1 && error[n]) {
            text[n] = error[n];
            ++n;
        }
    if (entropy_ready != ready || strcmp(text, entropy_message)) {
        entropy_ready = ready;
        memcpy(entropy_message, text, sizeof(text));
        dirty = true;
    }
}

bool page_host_browser_command(unsigned slot, struct user_browser_command *out) {
    struct page *p = page(slot);
    if (!p || !out || !p->command.action) return false;
    *out = p->command;
    p->command.action = 0;
    return true;
}

int32_t page_host_browser_state(unsigned slot, const struct user_browser_state *state) {
    struct page *p = page(slot);
    if (!display || !p) return USER_ENODEV;
    if (!state || state->flags & ~BROWSER_CAN_GO_BACK ||
        !memchr(state->url, 0, sizeof(state->url)) ||
        !memchr(state->title, 0, sizeof(state->title)))
        return USER_EINVAL;
    for (const char *p = state->url; *p; ++p)
        if ((unsigned char)*p < 32 || *p == 127) return USER_EINVAL;
    if (memcmp(&p->browser, state, sizeof(*state))) {
        p->browser = *state;
        for (char *text = p->browser.title; *text; ++text)
            if ((unsigned char)*text < 32 || *text == 127) *text = ' ';
        if (panel) dirty = true;
    }
    return 0;
}

static void browser_panel(bool open) {
    desktop_wifi_close();
    panel = open || !page(selected);
    address_edit = false;
    panel_press = toolbar_press = -1;
    browser_message = 0;
    if (panel) set_focus(-1);
    else set_focus((int)selected);
    update_keyboard();
    dirty = true;
}

static bool address_prefix(const char *text, const char *prefix, size_t length) {
    for (size_t i = 0; prefix[i]; ++i) {
        if (i >= length) return false;
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 'a' - 'A');
        if (c != prefix[i]) return false;
    }
    return true;
}

static bool navigate_address(char out[BROWSER_URL_MAX + 1]) {
    const char *start = address;
    while (*start == ' ') ++start;
    size_t length = strlen(start);
    while (length && start[length - 1] == ' ') --length;
    if (!length) return false;
    bool absolute =
        address_prefix(start, "https://", length) || address_prefix(start, "http://", length) ||
        (length == 11 && address_prefix(start, "about:", length) && !memcmp(start + 6, "blank", 5));
    unsigned prefix = absolute ? 0 : 8;
    if (length + prefix > BROWSER_URL_MAX) return false;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)start[i] <= 32 || start[i] == 127) return false;
    if (!absolute) {
        /* An unsupported explicit scheme must never become executable content. */
        for (size_t i = 0; i + 2 < length; ++i)
            if (start[i] == ':' && start[i + 1] == '/' && start[i + 2] == '/') return false;
        if (*start != '[')
            for (size_t i = 0; i < length && start[i] != '/' && start[i] != '?' && start[i] != '#';
                 ++i)
                if (start[i] == ':') {
                    size_t port = i + 1;
                    unsigned value = 0;
                    if (port == length) return false;
                    for (; port < length && start[port] != '/' && start[port] != '?' &&
                           start[port] != '#';
                         ++port) {
                        if (start[port] < '0' || start[port] > '9') return false;
                        value = value * 10 + (start[port] - '0');
                        if (value > 65535) return false;
                    }
                    if (!value) return false;
                    break;
                }
        memcpy(out, "https://", 8);
    }
    memcpy(out + prefix, start, length);
    out[length + prefix] = 0;
    return true;
}

/* One instance each of an app and the updater: its button shows the running
 * one, or starts it in a new slot. */
static void open_app(const struct app *app) {
    for (struct page *p = pages; p; p = p->next)
        if (p->app == app) {
            page_host_select(p->slot);
            return;
        }
    int32_t slot = process_app_open(app);
    if (slot < 0) {
        bool memory = slot == USER_ENOMEM;
        if (app->system)
            browser_message = memory ? "NOT ENOUGH MEMORY TO OPEN UPDATES" : "UPDATES UNAVAILABLE";
        else browser_message = memory ? "NOT ENOUGH MEMORY TO OPEN APP" : "APP UNAVAILABLE";
        dirty = true;
        return;
    }
    page_host_select((unsigned)slot);
}

static void launch_app(unsigned index) {
    const struct app *app = app_at(index);
    if (app) open_app(app);
}

static void browser_action(unsigned action) {
    if (action >= ACTION_PAGE) {
        page_host_select(action - ACTION_PAGE);
        return;
    }
    if (action >= ACTION_APP) {
        launch_app(action - ACTION_APP);
        return;
    }
    if (action == ACTION_UPDATES) {
        if (apps_updater()) open_app(apps_updater());
        else {
            browser_message = "UPDATES UNAVAILABLE";
            dirty = true;
        }
        return;
    }
    if (action == ACTION_WIFI) {
        soft_cancel();
        address_edit = false;
        desktop_wifi_open();
        update_keyboard();
        dirty = true;
        return;
    }
    if (action == ACTION_ADDRESS) {
        address_edit = true;
        address_all = true;
        update_keyboard();
        address_damage = true;
        return;
    }
    if (action == ACTION_CLOSE) {
        if (!panel && page(selected)) (void)process_web_control(selected, false);
        return;
    }
    if (action == ACTION_HOME) {
        page_host_home();
        return;
    }
    if (action == ACTION_BACK) {
        page_host_back();
        return;
    }
    if (action != ACTION_OPEN) return;
    char target[BROWSER_URL_MAX + 1];
    if (!navigate_address(target)) {
        browser_message = "ENTER A VALID WEB ADDRESS";
        dirty = true;
        return;
    }
    int32_t slot = process_web_open();
    if (slot < 0) {
        browser_message =
            slot == USER_ENODEV ? "BROWSER SERVICES UNAVAILABLE" : "NOT ENOUGH MEMORY TO OPEN PAGE";
        dirty = true;
        return;
    }
    struct page *p = page((unsigned)slot);
    p->command = (struct user_browser_command){.action = BROWSER_NAVIGATE};
    strcpy(p->command.url, target);
    strcpy(p->browser.url, target);
    p->browser.title[0] = 0;
    page_host_select((unsigned)slot);
}

static void address_key(const struct user_input_event *event, char text) {
    if (!(event->flags & INPUT_DOWN)) return;
    if (event->code == 1) {
        address_edit = false;
        update_keyboard();
        return;
    }
    if (!address_edit) return;
    if (event->code == 28 || event->code == 96) {
        browser_action(ACTION_OPEN);
        return;
    }
    if (event->code == 30 && (event->modifiers & INPUT_CTRL)) {
        address_all = true;
        address_damage = true;
        return;
    }
    if (event->code == 105 || event->code == 106 || event->code == 102 || event->code == 107) {
        if (event->code == 102) address_cursor = 0;
        else if (event->code == 107) address_cursor = address_length;
        else if (event->code == 105 && address_cursor) --address_cursor;
        else if (event->code == 106 && address_cursor < address_length) ++address_cursor;
        address_all = false;
        address_damage = true;
        return;
    }
    if (!text) text = screen_keyboard_text(event);
    if (event->code == 14 || event->code == 111 || (text >= 32 && text != 127)) {
        if (address_all) {
            address[0] = 0;
            address_length = address_cursor = 0;
            address_all = false;
        } else if (event->code == 14 && address_cursor) {
            memmove(address + address_cursor - 1, address + address_cursor,
                    address_length - address_cursor + 1);
            --address_cursor;
            --address_length;
        } else if (event->code == 111 && address_cursor < address_length) {
            memmove(address + address_cursor, address + address_cursor + 1,
                    address_length - address_cursor);
            --address_length;
        }
        if (text >= 32 && text != 127 && event->code != 14 && event->code != 111 &&
            address_length < BROWSER_URL_MAX) {
            memmove(address + address_cursor + 1, address + address_cursor,
                    address_length - address_cursor + 1);
            address[address_cursor++] = text;
            ++address_length;
        }
        address_damage = true;
        if (browser_message) {
            browser_message = 0;
            dirty = true;
        }
    }
}

static void address_render(struct framebuffer *screen) {
    unsigned top = address_top();
    gfx_rect(screen, 16, top, 336, 48, address_all ? 0x32656d : 0xf1f5f6);
    if (address_cursor < address_offset) address_offset = address_cursor;
    if (address_cursor > address_offset + 25) address_offset = address_cursor - 25;
    char line[27];
    unsigned n = 0;
    while (n < 26 && address_offset + n < address_length) {
        line[n] = address[address_offset + n];
        ++n;
    }
    line[n] = 0;
    gfx_text(screen, 24, top + 16, line, 2, address_all ? 0xf1f5f6 : 0x17232c);
    if (address_edit && !address_all)
        gfx_rect(screen, 24 + (address_cursor - address_offset) * 12, top + 12, 2, 24, 0x17232c);
}

static void page_list_render(struct framebuffer *screen, unsigned x, unsigned y) {
    (void)x;
    (void)y;
    if (!panel || desktop_wifi_visible()) return;
    if (!list_rows()) return;
    gfx_rect(screen, 0, y, 480, (int)list_bottom() - (int)y, 0x17232c);
    unsigned index = 0, row = 0;
    for (struct page *p = pages; p && row < list_rows(); p = p->next) {
        if (p->app || index++ < page_offset) continue;
        unsigned y = list_top() + row++ * LIST_ROW;
        gfx_rect(screen, 16, y, 448, 48, 0x31515c);
        char line[70];
        const char *label = p->browser.title[0] ? p->browser.title : p->browser.url;
        unsigned n = 0;
        while (n < 35 && label[n]) {
            line[n] = label[n];
            ++n;
        }
        line[n] = 0;
        gfx_text(screen, 26, y + 7, line[0] ? line : "LOADING PAGE...", 2, 0xf1f5f6);
        n = 0;
        while (n < 69 && p->browser.url[n]) {
            line[n] = p->browser.url[n];
            ++n;
        }
        line[n] = 0;
        gfx_text(screen, 26, y + 31, line, 1, 0xb9c9d0);
    }
}

static void launcher_render(struct framebuffer *screen) {
    if (!apps_count()) return;
    unsigned top = launcher_top();
    gfx_text(screen, 20, (int)top - 24, "APPS", 2, 0xf1f5f6);
    for (unsigned i = 0; i < apps_count(); ++i) {
        const struct app *app = app_at(i);
        int x = 16 + (int)(i % APP_COLUMNS * (APP_BUTTON + APP_GAP)),
            y = (int)(top + i / APP_COLUMNS * (APP_BUTTON + APP_GAP));
        gfx_rect(screen, x, y, APP_BUTTON, APP_BUTTON, 0x31515c);
        gfx_image(screen, x + (APP_BUTTON - APP_ICON_SIZE) / 2, y + 8, APP_ICON_SIZE, APP_ICON_SIZE,
                  app->icon);
        gfx_text(screen, x + (APP_BUTTON - gfx_text_width(app->name, 1)) / 2, y + 62, app->name, 1,
                 0xf1f5f6);
        bool running = false;
        for (struct page *p = pages; p; p = p->next)
            if (p->app == app) running = true;
        if (running) gfx_rect(screen, x + 28, y + 74, APP_BUTTON - 56, 3, 0x61e0bd);
    }
}

static void header_button(struct framebuffer *screen, unsigned x, const char *text) {
    gfx_rect(screen, (int)x, 6, HEADER_BUTTON, 36, 0x31515c);
    /* Characters advance 12 pixels at scale 2. */
    gfx_text(screen, (int)(x + (HEADER_BUTTON - 12 * strlen(text)) / 2), 16, text, 2, 0xf1f5f6);
}

static void page_header_render(struct framebuffer *screen, unsigned x, unsigned y) {
    (void)x;
    (void)y;
    gfx_rect(screen, 0, 0, 352, WEB_TOP, 0x17232c);
    if (panel) gfx_text(screen, 16, 16, "SOS2", 2, 0xf1f5f6);
    else header_button(screen, CLOSE_X, "CLOSE");
    /* Elsewhere Home and Back are keys: the phone's, or Alt+Home and Alt+Left in QEMU. */
    if (!navigation_buttons()) return;
    header_button(screen, HOME_X, "HOME");
    header_button(screen, BACK_X, "BACK");
}

/* The header's right side: the clock (or the volume while it changes) at
 * x 354, then, 6 pixels on as the buttons are spaced, the battery, whose
 * outline is 44x22 so that "100" fits inside at the clock's size, level with
 * the clock's digits. */
enum { CLOCK_X = 354, BATTERY_X = 418, BATTERY_Y = 12, BATTERY_W = 44, BATTERY_H = 22 };

/* A bar, filled to the level: white for the speakers, green for
 * headphones. */
static void volume_render(struct framebuffer *screen) {
    unsigned color = volume_headphones ? 0x61e0bd : 0xf1f5f6;
    gfx_rect(screen, CLOCK_X, 18, 58, 14, color);
    gfx_rect(screen, CLOCK_X + 2, 20, 54, 10, 0x17232c);
    if (volume_level) gfx_rect(screen, CLOCK_X + 3, 21, 52 * volume_level / volume_steps, 8, color);
}

/* The outline is green on a charger and red at 15 % or less. While the
 * battery charges, a plus on its level bar; otherwise the percentage, in
 * white at the clock's size over the level bar in a darker shade of the
 * outline, so the digits read on the bar and beside it alike. Without a
 * gauge, an empty outline. */
static void battery_render(struct framebuffer *screen) {
    const struct battery_view *b = &status_battery;
    bool low = b->percent <= 15;
    unsigned color = !b->available ? 0xb9c9d0 : b->charger ? 0x61e0bd : low ? 0xef7766 : 0xf1f5f6;
    gfx_rect(screen, BATTERY_X, BATTERY_Y, BATTERY_W, BATTERY_H, color);
    gfx_rect(screen, BATTERY_X + 2, BATTERY_Y + 2, BATTERY_W - 4, BATTERY_H - 4, 0x17232c);
    gfx_rect(screen, BATTERY_X + BATTERY_W, BATTERY_Y + 7, 3, BATTERY_H - 14, color);
    if (!b->available) return;
    int bar = (int)((BATTERY_W - 6) * b->percent / 100);
    if (b->charging) {
        gfx_rect(screen, BATTERY_X + 3, BATTERY_Y + 3, bar, BATTERY_H - 6, color);
        int x = BATTERY_X + BATTERY_W / 2, y = BATTERY_Y + BATTERY_H / 2;
        gfx_rect(screen, x - 2, y - 6, 4, 12, 0x17232c);
        gfx_rect(screen, x - 6, y - 2, 12, 4, 0x17232c);
        gfx_rect(screen, x - 1, y - 5, 2, 10, color);
        gfx_rect(screen, x - 5, y - 1, 10, 2, color);
        return;
    }
    gfx_rect(screen, BATTERY_X + 3, BATTERY_Y + 3, bar, BATTERY_H - 6,
             b->charger ? 0x1f5c4e
             : low      ? 0x73302a
                        : 0x31515c);
    char digits[12];
    *decimal(digits, b->percent) = 0;
    gfx_text(screen, BATTERY_X + (BATTERY_W - gfx_text_width(digits, 2)) / 2, BATTERY_Y + 4, digits,
             2, 0xf1f5f6);
}

static void status_render(struct framebuffer *screen, unsigned x, unsigned y) {
    (void)x;
    (void)y;
    gfx_rect(screen, 352, 0, 128, WEB_TOP, 0x17232c);
    if (volume_until) volume_render(screen);
    char time[6] = "--:--";
    if (status_minute != UINT64_MAX) {
        unsigned hour = (unsigned)(status_minute / 60 % 24),
                 minute = (unsigned)(status_minute % 60);
        time[0] = '0' + hour / 10;
        time[1] = '0' + hour % 10;
        time[3] = '0' + minute / 10;
        time[4] = '0' + minute % 10;
    }
    if (!volume_until) gfx_text(screen, CLOCK_X, 16, time, 2, 0xf1f5f6);
    battery_render(screen);
}

static void status_tick(void) {
    uint64_t now = loop_time_us();
    if (volume_until && now >= volume_until) {
        volume_until = 0;
        page_host_redraw_region(352, 0, 128, WEB_TOP, status_render);
    }
    if (now < status_next) return;
    status_next = now + STATUS_PERIOD_US;
    uint64_t us = 0, minute = system_clock_read(&us) ? us / 60000000 : UINT64_MAX;
    struct battery_view battery = *battery_view();
    if (battery.percent > 100) battery.available = false;
    if (minute != status_minute || battery.available != status_battery.available ||
        battery.percent != status_battery.percent || battery.charger != status_battery.charger ||
        battery.charging != status_battery.charging) {
        status_minute = minute;
        status_battery = battery;
        page_host_redraw_region(352, 0, 128, WEB_TOP, status_render);
    }
}

void page_host_volume(unsigned level, unsigned steps, bool headphones) {
    volume_level = level;
    volume_steps = steps ? steps : 1;
    volume_headphones = headphones;
    volume_until = time_us() + VOLUME_SHOWN_US;
    page_host_redraw_region(352, 0, 128, WEB_TOP, status_render);
}

static void browser_render(struct framebuffer *screen) {
    if (!panel) return;
    if (desktop_wifi_visible()) {
        desktop_wifi_render(screen);
        return;
    }
    gfx_rect(screen, 0, WEB_TOP, 480, 800 - WEB_TOP, 0x17232c);
    unsigned y = STATUS_TOP;
    gfx_text(screen, 20, y,
             storage_session ? "TEMPORARY SESSION / DATA CLEARS ON REBOOT"
             : storage_ready ? "PERSISTENT STORAGE READY"
                             : "PERSISTENT STORAGE UNAVAILABLE",
             1, 0xf1f5f6);
    y += 18;
    if (!entropy_ready) {
        gfx_text(screen, 20, y, "SECURE RANDOMNESS UNAVAILABLE", 1, 0x61e0bd);
        y += 18;
    }
    if (!entropy_ready && entropy_message[0]) {
        gfx_text(screen, 20, y, entropy_message, 1, 0x61e0bd);
        y += 18;
    }
    for (unsigned i = 0; i < DEVICES; ++i) {
        if (!device_message[i][0]) continue;
        gfx_text(screen, 20, y, device_message[i], 1, 0x61e0bd);
        y += 18;
    }
    y = settings_top();
    static const char *const buttons[] = {"WI-FI", "UPDATES"};
    for (unsigned i = 0; i < 2; ++i) {
        int x = i ? 248 : 16;
        gfx_rect(screen, x, y, 216, 44, 0x31515c);
        gfx_text(screen, x + (216 - gfx_text_width(buttons[i], 2)) / 2, y + 14, buttons[i], 2,
                 0xf1f5f6);
    }
    /* The updater running, marked as the launcher marks a running app. */
    for (struct page *p = pages; p; p = p->next)
        if (p->app && p->app == apps_updater()) gfx_rect(screen, 328, y + 38, 56, 3, 0x61e0bd);
    gfx_text(screen, 20, y + 68, "WEB PAGES", 2, 0xf1f5f6);
    address_render(screen);
    y = address_top();
    gfx_rect(screen, 364, y, 100, 48, 0x31515c);
    gfx_text(screen, 390, y + 16, "OPEN", 2, 0xf1f5f6);
    if (browser_message) gfx_text(screen, 20, y + 64, browser_message, 1, 0x61e0bd);
    page_list_render(screen, 0, list_top());
    launcher_render(screen);
}

/* The current pass's next band, and once it is the last, the region shown. */
static void web_pass_step(void) {
    if (web_pass.next >= web_pass.bottom) return;
    unsigned rows = WEB_BAND_PIXELS / web_pass.width;
    if (!rows) rows = 1;
    unsigned end = web_pass.bottom - web_pass.next > rows ? web_pass.next + rows : web_pass.bottom;
    (void)display_draw_region(display, &composition, web_pass.left, web_pass.next + WEB_TOP,
                              web_pass.width, end - web_pass.next);
    web_pass.next = end;
    if (end == web_pass.bottom)
        display_show_region(display, &composition, web_pass.left, web_pass.top + WEB_TOP,
                            web_pass.width, web_pass.bottom - web_pass.top);
}

void page_host_render(void) {
    status_tick();
    screen_keyboard_tick(&keyboard, loop_time_us(), soft_emit, 0);
    if (!display || (!dirty && !address_damage && !keyboard_damage && !widget_count &&
                     !web_damage.right && web_pass.next >= web_pass.bottom))
        return;
    /* A phone scanout is uncached. Comparing a whole frame to highlight one
     * key can exceed the touch report deadline and turn its release into a
     * cancellation. Paint key/entry damage directly, and postpone unrelated
     * full redraws until the control gesture ends, including settings buttons
     * and fields. Web drags still paint. No touch deadline is relaxed. */
    bool holding_control =
        pressed && (panel || toolbar_press >= 0 || (keyboard.visible && keyboard.pressed >= 0));
    /* Drivers may cache the selected page's view. A page drawing CPU frames is
     * cached only unobscured and at full height: keyboard and settings
     * overlays continue through the general compositor path. GPU frames never
     * enter the composition, so the display always draws them from the page's
     * snapshot, above the keyboard if it is up. Slots never expose GPU IDs. */
    struct page *visible_page = panel ? 0 : page(selected);
    bool gpu_view = visible_page && visible_page->ready && visible_page->gpu_frames;
    uint32_t view = gpu_view || (visible_page && visible_page->ready && !keyboard.visible &&
                                 visible_page->viewport == WEB_HEIGHT)
                        ? selected
                        : UINT32_MAX;
    unsigned view_height = gpu_view ? visible_page->viewport : WEB_HEIGHT;
    if (!dirty || (frame_presented && holding_control)) {
        /* No full redraw: the composition still shows composed_view. A GPU
         * driver given the new view now would cache those stale pixels (e.g. a
         * just-hidden keyboard) as the page, so use the general path until the
         * postponed redraw composes it. A page update covering the whole
         * viewport (e.g. the first frame after Open) recomposes the page area
         * below, so the driver may cache that view again. */
        if (view != UINT32_MAX && !gpu_view && web_damage.right >= WEB_WIDTH && !web_damage.left &&
            !web_damage.top && web_damage.bottom >= visible_page->viewport)
            composed_view = view;
        display_page_view(gpu_view || view == composed_view ? view : UINT32_MAX, view_height);
        /* Damage that comes during a pass waits for the next. */
        if (web_damage.right && web_pass.next >= web_pass.bottom) {
            unsigned height = visible_page ? visible_page->viewport : 0;
            unsigned bottom = web_damage.bottom < height ? web_damage.bottom : height;
            if (visible_page && visible_page->ready && web_damage.top < bottom) {
                unsigned x = web_damage.left, y = web_damage.top + WEB_TOP,
                         width = web_damage.right - x, rows = bottom - web_damage.top;
                /* The display already holds GPU frames: show them, copy nothing. */
                if (visible_page->gpu_frames) display_damage(x, y, width, rows);
                else {
                    for (unsigned row = web_damage.top; row < bottom; ++row)
                        memcpy(composition_pixels + (row + WEB_TOP) * WEB_WIDTH + x,
                               visible_page->pixels + row * WEB_WIDTH + x, width * 4);
                    if (display_bands())
                        web_pass = (__typeof__(web_pass)){x, width, web_damage.top, web_damage.top,
                                                          bottom};
                    else (void)display_present_region(display, &composition, x, y, width, rows);
                }
            }
            web_damage.right = web_damage.bottom = 0;
        }
        web_pass_step();
        for (unsigned i = 0; i < widget_count; ++i) {
            const struct widget_damage *d = &widget_damage[i];
            d->paint(&composition, d->x, d->y);
            (void)display_present_region(display, &composition, d->x, d->y, d->width, d->height);
        }
        widget_count = 0;
        if (address_damage && panel && !desktop_wifi_visible()) {
            address_render(&composition);
            (void)display_present_region(display, &composition, 16, address_top(), 336, 48);
        }
        if (keyboard.visible) {
            struct screen_key key;
            for (unsigned id = 0; id < 64 && screen_keyboard_key(&keyboard, id, &key); ++id) {
                if (!(keyboard_damage & (UINT64_C(1) << id))) continue;
                struct framebuffer tile = composition;
                tile.pixels = (unsigned char *)composition.pixels +
                              (size_t)key.y * composition.stride + (size_t)key.x * 4;
                tile.width = key.w;
                tile.height = key.h;
                screen_keyboard_render(&keyboard, &tile, key.x, key.y);
                (void)display_present_region(display, &composition, key.x, key.y, key.w, key.h);
            }
        }
        address_damage = false;
        keyboard_damage = 0;
        return;
    }
    /* This full redraw composes the view before the driver's next poll. The
     * composition holds the page's pixels only for CPU frames. */
    display_page_view(view, view_height);
    composed_view = gpu_view ? UINT32_MAX : view;
    struct framebuffer *physical = display;
    struct framebuffer *display = &composition;
    gfx_rect(display, 0, 0, WEB_WIDTH, 800, 0x17232c);
    page_header_render(display, 0, 0);
    if (!panel) {
        struct page *p = page(selected);
        if (p && p->ready && p->gpu_frames) {
            /* The display draws GPU frames from the page's snapshot. */
        } else if (p && p->ready) {
            struct framebuffer page_frame = {p->pixels, WEB_WIDTH, p->viewport, WEB_WIDTH * 4,
                                             PIXEL_XRGB8888};
            struct framebuffer target = *display;
            target.pixels = (unsigned char *)display->pixels + WEB_TOP * display->stride;
            target.width = WEB_WIDTH;
            target.height = p->viewport;
            (void)gfx_present(&target, &page_frame);
        } else {
            gfx_rect(display, 0, WEB_TOP, WEB_WIDTH, WEB_HEIGHT, 0xf5f1e8);
            gfx_text(display, 40, 140, "LOADING PAGE...", 2, 0x17232c);
        }
    }
    status_render(display, 352, 0);
    browser_render(display);
    screen_keyboard_render(&keyboard, display, 0, 0);
    (void)display_present(physical, display);
    frame_presented = true;
    address_damage = false;
    keyboard_damage = 0;
    widget_count = 0;
    dirty = false;
    web_damage.right = web_damage.bottom = 0;
    web_pass.next = web_pass.bottom = 0;
}
