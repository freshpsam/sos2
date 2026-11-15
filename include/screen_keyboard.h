#ifndef SOS2_SCREEN_KEYBOARD_H
#define SOS2_SCREEN_KEYBOARD_H
#include "graphics.h"
#include "user_input.h"

enum screen_key_action { SCREEN_KEY, SCREEN_SHIFT, SCREEN_SYMBOLS, SCREEN_HIDE };

struct screen_key {
    int id, x, y, w, h;
    unsigned code;
    bool shift;
    enum screen_key_action action;
    char label[8], text;
};

/* One state per input seat. Geometry uses the complete display, even when the
 * renderer receives a clipped damage view. No platform, clock or page owner is
 * embedded here; consumers route emitted keys to their own focused surface. */
struct screen_keyboard {
    uint32_t width, height;
    bool visible, symbols, shift, down, gesture, backspace;
    int pressed;
    uint64_t repeat_at;
    struct screen_key key;
};

/* text is the displayed US-layout character on keydown (including \b/\n), or
 * zero on keyup. Web renderers use the physical event and their own editor. */
typedef void (*screen_keyboard_emit)(void *, const struct user_input_event *, char text);
void screen_keyboard_init(struct screen_keyboard *, uint32_t width, uint32_t height);
char screen_keyboard_text(const struct user_input_event *);
uint32_t screen_keyboard_top(const struct screen_keyboard *);
bool screen_keyboard_key(const struct screen_keyboard *, unsigned id, struct screen_key *);
void screen_keyboard_cancel(struct screen_keyboard *, screen_keyboard_emit, void *);
void screen_keyboard_show(struct screen_keyboard *, bool visible, screen_keyboard_emit, void *);
/* Observe every pointer report, including outside/hidden keyboard reports, so
 * dragging into the keyboard cannot become a new key press. A consumed gesture
 * remains consumed until release, even if it leaves the keyboard or hides it. */
bool screen_keyboard_handle(struct screen_keyboard *, const struct pointer_event *, uint64_t now,
                            screen_keyboard_emit, void *);
void screen_keyboard_tick(struct screen_keyboard *, uint64_t now, screen_keyboard_emit, void *);
void screen_keyboard_render(const struct screen_keyboard *, struct framebuffer *, int origin_x,
                            int origin_y);
#endif
