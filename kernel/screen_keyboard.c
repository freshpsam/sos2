#include "screen_keyboard.h"
#include "platform.h"
#include <limits.h>

/* Shared pointer/touch UI: no QEMU or hardware-keyboard dependency. The page
 * viewport stops above this trusted surface; pages cannot draw over its keys. */
static const struct {
    unsigned code;
    char normal, upper;
} symbols_row[] = {
    {12, '-', 0}, {53, '/', 0}, {39, ':', 1}, {39, ';', 0}, {10, '(', 1},
    {11, ')', 1}, {5, '$', 1},  {8, '&', 1},  {3, '@', 1},
};

static const struct {
    unsigned code;
    char text;
    bool shift;
} extra_symbols[] = {
    {26, '[', false}, {27, ']', false},  {26, '{', true},  {27, '}', true}, {51, '<', true},
    {52, '>', true},  {43, '\\', false}, {13, '=', false}, {12, '_', true}, {41, '~', true},
    {41, '`', false}, {40, '\'', false}, {40, '"', true},  {9, '*', true},  {13, '+', true},
    {12, '-', false}, {53, '/', false},  {39, ':', true},  {3, '@', true},
};

static bool key_at(unsigned id, bool symbols, bool shift, struct screen_key *key) {
    *key = (struct screen_key){.id = (int)id, .h = 48, .action = SCREEN_KEY};
    if (id < 10) {
        key->x = 4 + 48 * id;
        key->y = 554;
        key->w = 40;
        key->code = symbols ? id + 2 : id + 16;
        key->label[0] = symbols ? "1234567890"[id] : "qwertyuiop"[id];
    } else if (id < 19) {
        unsigned col = id - 10;
        key->x = 28 + 48 * col;
        key->y = 610;
        key->w = 40;
        key->code = symbols ? symbols_row[col].code : 30 + col;
        key->shift = symbols && symbols_row[col].upper;
        key->label[0] = symbols ? symbols_row[col].normal : "asdfghjkl"[col];
    } else if (id < 28) {
        unsigned col = id - 19;
        key->y = 666;
        if (!col) {
            key->x = 4;
            key->w = 64;
            key->action = SCREEN_SHIFT;
            strcpy(key->label, symbols ? (shift ? "123" : "#+=") : "SHIFT");
        } else if (col == 8) {
            key->x = 412;
            key->w = 64;
            key->code = 14;
            strcpy(key->label, "BACK");
        } else {
            key->x = 76 + (col - 1) * 48;
            key->w = 40;
            static const unsigned codes[] = {2, 53, 4, 7, 6, 13, 43};
            key->code = symbols ? codes[col - 1] : 44 + col - 1;
            key->shift = symbols;
            key->label[0] = symbols ? "!?#^%+|"[col - 1] : "zxcvbnm"[col - 1];
        }
    } else if (id < 33) {
        static const int positions[] = {4, 80, 128, 316, 364}, widths[] = {68, 40, 180, 40, 112};
        unsigned col = id - 28;
        key->x = positions[col];
        key->y = 722;
        key->w = widths[col];
        if (!col) {
            key->action = SCREEN_SYMBOLS;
            strcpy(key->label, symbols ? "ABC" : "?123");
        } else if (col == 1) {
            key->code = 51;
            strcpy(key->label, ",");
        } else if (col == 2) {
            key->code = 57;
            strcpy(key->label, "SPACE");
        } else if (col == 3) {
            key->code = 52;
            strcpy(key->label, ".");
        } else {
            key->code = 28;
            strcpy(key->label, "ENTER");
        }
    } else if (id == 33) {
        key->x = 400;
        key->y = 522;
        key->w = 76;
        key->h = 26;
        key->action = SCREEN_HIDE;
        strcpy(key->label, "HIDE");
    } else return false;
    if (symbols && shift && id < 19) {
        key->code = extra_symbols[id].code;
        key->label[0] = extra_symbols[id].text;
        key->shift = extra_symbols[id].shift;
    }
    if (!symbols && key->label[0] >= 'a' && key->label[0] <= 'z') {
        key->shift = shift;
        if (shift) key->label[0] -= 'a' - 'A';
    }
    if (key->action == SCREEN_KEY) {
        if (!key->label[1]) key->text = key->label[0];
        else if (key->code == 57) key->text = ' ';
        else if (key->code == 14) key->text = '\b';
        else if (key->code == 28) key->text = '\n';
    }
    return true;
}

static int sx(const struct screen_keyboard *keyboard, int x) {
    return (int)((int64_t)x * keyboard->width / 480);
}

static int sy(const struct screen_keyboard *keyboard, int y) {
    return (int)((int64_t)y * keyboard->height / 800);
}

void screen_keyboard_init(struct screen_keyboard *keyboard, uint32_t width, uint32_t height) {
    *keyboard = (struct screen_keyboard){.pressed = -1};
    if (width > INT_MAX || height > INT_MAX) return;
    keyboard->width = width;
    keyboard->height = height;
}

uint32_t screen_keyboard_top(const struct screen_keyboard *keyboard) {
    return (uint32_t)sy(keyboard, 520);
}

bool screen_keyboard_key(const struct screen_keyboard *keyboard, unsigned id,
                         struct screen_key *key) {
    if (!key_at(id, keyboard->symbols, keyboard->shift, key)) return false;
    int right = sx(keyboard, key->x + key->w), bottom = sy(keyboard, key->y + key->h);
    key->x = sx(keyboard, key->x);
    key->y = sy(keyboard, key->y);
    key->w = right - key->x;
    key->h = bottom - key->y;
    return true;
}

static bool screen_keyboard_hit(const struct screen_keyboard *keyboard, uint32_t x, uint32_t y,
                                struct screen_key *key) {
    if (x >= keyboard->width || y >= keyboard->height) return false;
    for (unsigned id = 0; screen_keyboard_key(keyboard, id, key); ++id)
        if (x >= (unsigned)key->x && x < (unsigned)(key->x + key->w) && y >= (unsigned)key->y &&
            y < (unsigned)(key->y + key->h))
            return true;
    return false;
}

static void emit_key(const struct screen_key *key, unsigned flags, screen_keyboard_emit emit,
                     void *context) {
    if (!emit) return;
    struct user_input_event event = {.type = INPUT_KEY,
                                     .code = key->code,
                                     .flags = flags,
                                     .modifiers =
                                         (flags & INPUT_DOWN) && key->shift ? INPUT_SHIFT : 0};
    emit(context, &event, flags & INPUT_DOWN ? key->text : 0);
}

void screen_keyboard_cancel(struct screen_keyboard *keyboard, screen_keyboard_emit emit,
                            void *context) {
    bool release = keyboard->backspace;
    struct screen_key key = keyboard->key;
    keyboard->pressed = -1;
    keyboard->backspace = false;
    /* Keep down/gesture until the real release. Focus/layout changes must not
     * let a still-held finger acquire a second key or a different surface. */
    if (release) emit_key(&key, 0, emit, context);
}

void screen_keyboard_show(struct screen_keyboard *keyboard, bool visible, screen_keyboard_emit emit,
                          void *context) {
    if (keyboard->visible == visible) return;
    screen_keyboard_cancel(keyboard, emit, context);
    keyboard->visible = visible && keyboard->width && keyboard->height;
}

bool screen_keyboard_handle(struct screen_keyboard *keyboard, const struct pointer_event *event,
                            uint64_t now, screen_keyboard_emit emit, void *context) {
    bool press = !keyboard->down && event->down && !event->cancelled;
    bool release = keyboard->down && !event->down && !event->cancelled;
    bool owned = keyboard->gesture;
    keyboard->down = event->down && !event->cancelled;
    if (event->cancelled) {
        screen_keyboard_cancel(keyboard, emit, context);
        keyboard->gesture = false;
        return owned;
    }
    if (!keyboard->down) keyboard->gesture = false;
    if (!keyboard->visible) return owned;
    if (keyboard->pressed >= 0) {
        struct screen_key hit;
        bool same =
            screen_keyboard_hit(keyboard, event->x, event->y, &hit) && hit.id == keyboard->pressed;
        if (!same) screen_keyboard_cancel(keyboard, emit, context);
        else if (release) {
            struct screen_key key = keyboard->key;
            bool backspace = keyboard->backspace;
            screen_keyboard_cancel(keyboard, emit, context);
            if (key.action == SCREEN_HIDE) keyboard->visible = false;
            else if (key.action == SCREEN_SHIFT) keyboard->shift = !keyboard->shift;
            else if (key.action == SCREEN_SYMBOLS) {
                keyboard->symbols = !keyboard->symbols;
                keyboard->shift = false;
            } else {
                if (!keyboard->symbols) keyboard->shift = false;
                if (!backspace) {
                    emit_key(&key, INPUT_DOWN, emit, context);
                    emit_key(&key, 0, emit, context);
                }
            }
        }
        return true;
    }
    if (owned) return true;
    if (event->x >= keyboard->width || event->y < screen_keyboard_top(keyboard) ||
        event->y >= keyboard->height)
        return false;
    if (press) {
        keyboard->gesture = true;
        if (screen_keyboard_hit(keyboard, event->x, event->y, &keyboard->key)) {
            keyboard->pressed = keyboard->key.id;
            if (keyboard->key.code == 14) {
                keyboard->backspace = true;
                keyboard->repeat_at = now + 450000;
                emit_key(&keyboard->key, INPUT_DOWN, emit, context);
            }
        }
    }
    return true;
}

void screen_keyboard_tick(struct screen_keyboard *keyboard, uint64_t now, screen_keyboard_emit emit,
                          void *context) {
    if (keyboard->visible && keyboard->backspace && now >= keyboard->repeat_at) {
        keyboard->repeat_at = now + 60000;
        emit_key(&keyboard->key, INPUT_DOWN | INPUT_REPEAT, emit, context);
    }
}

static unsigned font_scale(const struct screen_keyboard *keyboard, unsigned scale) {
    unsigned x = (unsigned)(((uint64_t)scale * keyboard->width + 240) / 480);
    unsigned y = (unsigned)(((uint64_t)scale * keyboard->height + 400) / 800);
    unsigned result = x < y ? x : y;
    return result ? result : 1;
}

void screen_keyboard_render(const struct screen_keyboard *keyboard, struct framebuffer *screen,
                            int origin_x, int origin_y) {
    if (!keyboard->visible) return;
    int top = (int)screen_keyboard_top(keyboard);
    gfx_rect(screen, -origin_x, top - origin_y, (int)keyboard->width, (int)keyboard->height - top,
             0x1c2c35);
    gfx_rect(screen, -origin_x, top - origin_y, (int)keyboard->width, sy(keyboard, 522) - top,
             0x61e0bd);
    gfx_text(screen, sx(keyboard, 14) - origin_x, sy(keyboard, 532) - origin_y, "KEYBOARD",
             font_scale(keyboard, 1), 0xb9c9d0);
    struct screen_key key;
    for (unsigned id = 0; screen_keyboard_key(keyboard, id, &key); ++id) {
        bool accent =
            key.id == keyboard->pressed || (key.action == SCREEN_SHIFT && keyboard->shift);
        uint32_t fill = accent ? 0x61e0bd : key.code == 28 ? 0x396d67 : 0x344952;
        gfx_rect(screen, key.x - origin_x, key.y - origin_y, key.w, key.h, fill);
        unsigned scale = font_scale(keyboard, strlen(key.label) > 3 ? 1 : 2);
        int width = gfx_text_width(key.label, scale);
        gfx_text(screen, key.x + (key.w - width) / 2 - origin_x,
                 key.y + (key.h - 7 * (int)scale) / 2 - origin_y, key.label, scale,
                 accent ? 0x102820 : 0xf4f8fa);
    }
}

char screen_keyboard_text(const struct user_input_event *event) {
    if (event->modifiers & (INPUT_CTRL | INPUT_ALT | INPUT_META)) return 0;
    unsigned code = event->code;
    bool shift = event->modifiers & INPUT_SHIFT;
    char c = 0;
    if (code >= 16 && code <= 25) c = "qwertyuiop"[code - 16];
    else if (code >= 30 && code <= 38) c = "asdfghjkl"[code - 30];
    else if (code >= 44 && code <= 50) c = "zxcvbnm"[code - 44];
    if (c) return shift != !!(event->modifiers & INPUT_CAPS_LOCK) ? c - 'a' + 'A' : c;
    if (code >= 2 && code <= 11) return (shift ? "!@#$%^&*()" : "1234567890")[code - 2];
    const unsigned codes[] = {12, 13, 26, 27, 39, 40, 41, 43, 51, 52, 53, 57};
    const char *plain = "-=[];'`\\,./ ", *upper = "_+{}:\"~|<>? ";
    for (unsigned i = 0; i < sizeof(codes) / sizeof(*codes); ++i)
        if (code == codes[i]) return (shift ? upper : plain)[i];
    return 0;
}
