#include "desktop_wifi.h"
#include "wifi_setup.h"
#include "page_host.h"
#include "screen_keyboard.h"
#include "haptics.h"
#include "platform.h"

enum {
    BACK = 1,
    SCAN,
    CONNECT,
    FORGET,
    PASSWORD,
    SHOW,
    WPA2,
    WPA3,
    PREVIOUS,
    NEXT,
    SAVED,
    ROW = 100,
    ROWS = 6
};

enum {
    DETAIL_ACTIONS_Y = 144,
    DETAIL_PASSWORD_Y = 200,
    DETAIL_SECURITY_Y = 260,
    STATUS_X = 104,
    STATUS_Y = 68,
    STATUS_WIDTH = 360,
    STATUS_HEIGHT = 16
};

static bool visible, detail, editing, reveal, all;
static unsigned security, length, cursor, offset, page;
static int pressed = -1;
static char password[64];
static struct wifi_network selected, pressed_network;
static const char *message;
static char ip_address[16];
static void render_password(struct framebuffer *, unsigned, unsigned);
static void render_status(struct framebuffer *, unsigned, unsigned);

static void redraw_password(void) {
    page_host_redraw_region(16, DETAIL_PASSWORD_Y, 356, 48, render_password);
}

static void redraw_status(void) {
    page_host_redraw_region(STATUS_X, STATUS_Y, STATUS_WIDTH, STATUS_HEIGHT, render_status);
}

/* Typing over a selection empties the text and keeps Show/Hide as drawn. */
static void clear_text(void) {
    wifi_secret_clear(password, sizeof(password));
    length = cursor = offset = 0;
    all = false;
}

static void clear_password(void) {
    clear_text();
    reveal = false;
}

/* What the last full render drew: taps act on it, not on state the worker
 * changed since (a profile it saves turns Connect into Forget and moves the
 * nearby networks down a row). */
struct drawn_screen {
    bool valid, detail, forget, saved, previous, next;
    unsigned top, rows, paging_y;
    struct wifi_network saved_network, row[ROWS];
};
static struct drawn_screen drawn;

bool desktop_wifi_visible(void) { return visible; }

bool desktop_wifi_editing(void) { return visible && detail && editing; }

void desktop_wifi_hide_keyboard(void) { editing = false; }

void desktop_wifi_close(void) {
    clear_password();
    visible = detail = editing = false;
    pressed = -1;
    message = 0;
    drawn.valid = false;
}

void desktop_wifi_open(void) {
    desktop_wifi_close();
    visible = true;
    page = 0;
    page_host_redraw();
}

static bool same_name(const struct wifi_network *a, const struct wifi_network *b) {
    return a->ssid_length == b->ssid_length && !memcmp(a->ssid, b->ssid, a->ssid_length);
}

static unsigned preferred(unsigned modes) {
    return modes & (1u << WIFI_WPA3_SAE)   ? WIFI_WPA3_SAE
           : modes & (1u << WIFI_WPA2_PSK) ? WIFI_WPA2_PSK
                                           : WIFI_OPEN;
}

static bool saved_selection(void) {
    const struct wifi_setup_view *v = wifi_setup_view();
    return v->has_saved && same_name(&selected, &v->saved) && (v->saved.modes & (1u << security));
}

/* The field starts with the saved password when this network and mode are
 * the saved ones, so Show reveals it. */
static void load_saved(void) {
    clear_password();
    if (saved_selection()) length = cursor = wifi_setup_saved_password(password);
}

/* Whether the page holds the saved network's own credential: its saved
 * password in the field, or an empty field for a WPA2 profile imported as a
 * PMK alone. Then the one action is Forget, otherwise Connect. */
static bool saved_credential(void) {
    if (!saved_selection()) return false;
    char saved[64];
    unsigned n = wifi_setup_saved_password(saved);
    bool same = n == length && !memcmp(saved, password, n);
    wifi_secret_clear(saved, sizeof(saved));
    return same;
}

static void choose(const struct wifi_network *network) {
    if (!network->modes) {
        message = "Security not supported";
        return;
    }
    selected = *network;
    security = preferred(network->modes);
    const struct wifi_setup_view *v = wifi_setup_view();
    if (v->has_saved && same_name(network, &v->saved) && (network->modes & v->saved.modes))
        security = preferred(v->saved.modes);
    load_saved();
    detail = true;
    editing = security != WIFI_OPEN && !saved_selection();
    message = 0;
}

static const struct wifi_network *network_at(unsigned action) {
    if (!drawn.valid || drawn.detail) return 0;
    if (action == SAVED) return drawn.saved ? &drawn.saved_network : 0;
    return action >= ROW && action - ROW < drawn.rows ? &drawn.row[action - ROW] : 0;
}

/* Nearby networks start under the saved one, or right under the buttons.
 * Previous/Next follow the page's last row with the rows' 8 px gap. */
static unsigned list_top(void) { return wifi_setup_view()->has_saved ? 224 : 164; }

static bool no_networks(const struct wifi_setup_view *v) {
    return !v->radio.count && v->radio.scan_generation && v->radio.state != WIFI_RADIO_SCANNING;
}

static unsigned paging_y(void) {
    const struct wifi_setup_view *v = wifi_setup_view();
    unsigned first = page * ROWS, rows = first < v->radio.count ? v->radio.count - first : 0;
    if (rows > ROWS) rows = ROWS;
    return list_top() + (rows ? rows : no_networks(v) ? 1u : 0u) * 64;
}

static int hit(unsigned x, unsigned y) {
    if (!drawn.valid || x < 16 || x >= 464) return -1;
    if (!drawn.detail) {
        if (y >= 104 && y < 148) return x < 312 ? SCAN : BACK;
        if (drawn.saved && y >= 164 && y < 220) return SAVED;
        /* A short last page puts Previous where later rows would be. */
        if (y >= drawn.top && ((y - drawn.top) % 64) < 56 && (y - drawn.top) / 64 < drawn.rows)
            return (int)(ROW + (y - drawn.top) / 64);
        if (y >= drawn.paging_y && y < drawn.paging_y + 44) {
            if (x < 232 && drawn.previous) return PREVIOUS;
            if (x >= 248 && drawn.next) return NEXT;
        }
    } else {
        if (y >= DETAIL_ACTIONS_Y && y < DETAIL_ACTIONS_Y + 44)
            return x < 312 ? (drawn.forget ? FORGET : CONNECT) : BACK;
        if (y >= DETAIL_PASSWORD_Y && y < DETAIL_PASSWORD_Y + 48 && security != WIFI_OPEN)
            return x < 380 ? PASSWORD : SHOW;
        if (y >= DETAIL_SECURITY_Y && y < DETAIL_SECURITY_Y + 44) {
            if (x < 232 && (selected.modes & (1u << WIFI_WPA2_PSK))) return WPA2;
            if (x >= 248 && (selected.modes & (1u << WIFI_WPA3_SAE))) return WPA3;
        }
    }
    return -1;
}

static void action(unsigned action) {
    const struct wifi_setup_view *v = wifi_setup_view();
    message = 0;
    if (action == BACK) {
        clear_password();
        editing = false;
        if (detail) detail = false;
        else visible = false;
    } else if (action == SCAN) {
        if (wifi_setup_scan()) page = 0;
        else message = "Wi-Fi is busy or unavailable";
    } else if (action == PASSWORD) {
        editing = true;
        all = length != 0;
    } else if (action == SHOW) reveal = !reveal;
    else if (action == WPA2 || action == WPA3) {
        /* One passphrase serves both modes of a transition network: the field
         * keeps it, unless the new mode is the saved one. */
        unsigned mode = action == WPA2 ? WIFI_WPA2_PSK : WIFI_WPA3_SAE;
        if (mode != security) {
            security = mode;
            if (saved_selection()) load_saved();
            editing = !saved_selection();
        }
    } else if (action == PREVIOUS) {
        if (page) --page;
    } else if (action == NEXT) {
        if ((page + 1) * ROWS < v->radio.count) ++page;
    } else if (action == FORGET) {
        if (wifi_setup_forget()) {
            clear_password();
            editing = false;
            detail = false;
        } else message = "Could not forget; try again";
    } else if (action == CONNECT) {
        /* Enter also connects; with the saved credential, as it is saved. */
        bool saved = saved_credential();
        if (!v->storage && !v->session) message = "Storage unavailable";
        else if (!saved && security != WIFI_OPEN && length < (security == WIFI_WPA2_PSK ? 8u : 1u))
            message =
                security == WIFI_WPA2_PSK ? "Password: 8 to 63 characters" : "Enter the password";
        /* The field keeps the password, so Show/Hide still works; it is
         * wiped on leaving the network's page. */
        else if (wifi_setup_connect(&selected, security, (const unsigned char *)password,
                                    saved ? 0 : length, saved)) {
            editing = all = false;
            message = 0;
        } else message = "Wi-Fi is busy; try again";
    }
    page_host_redraw();
}

void desktop_wifi_pointer(const struct pointer_event *event, bool press, bool release) {
    if (!visible) return;
    if (event->cancelled) {
        pressed = -1;
        return;
    }
    int target = hit(event->x, event->y);
    if (press) {
        pressed = target;
        if (target >= 0 && target != PASSWORD) haptics_tap(); /* not for the field */
        const struct wifi_network *n = target >= 0 ? network_at((unsigned)target) : 0;
        if (n) pressed_network = *n;
    } else if (event->down && target != pressed) pressed = -1;
    if (release && target >= 0 && target == pressed) {
        const struct wifi_network *n = network_at((unsigned)target);
        if (n) {
            if (!memcmp(n, &pressed_network, sizeof(*n))) choose(n);
            page_host_redraw();
        }
        /* Forget acts on whatever is saved now: if the worker saved or
         * replaced a profile since the page was drawn, show the page again
         * instead. */
        else if ((target == CONNECT || target == FORGET) &&
                 (target == FORGET) != saved_credential())
            page_host_redraw();
        else if (target < ROW) action((unsigned)target);
    }
    if (!event->down) pressed = -1;
}

void desktop_wifi_key(const struct user_input_event *event, char text) {
    if (!visible || !(event->flags & INPUT_DOWN)) return;
    if (event->code == 1) {
        action(BACK);
        return;
    }
    if (!detail) return;
    if (event->code == 28 || event->code == 96) {
        action(CONNECT);
        return;
    }
    if (!editing || security == WIFI_OPEN) return;
    if (event->code == 30 && (event->modifiers & INPUT_CTRL)) {
        all = true;
        redraw_password();
        return;
    }
    if (event->code == 105 || event->code == 106 || event->code == 102 || event->code == 107) {
        if (event->code == 102) cursor = 0;
        else if (event->code == 107) cursor = length;
        else if (event->code == 105 && cursor) --cursor;
        else if (event->code == 106 && cursor < length) ++cursor;
        all = false;
        redraw_password();
        return;
    }
    if (!text) text = screen_keyboard_text(event);
    if (event->code == 14 || event->code == 111 || (text >= 32 && text < 127)) {
        const char *previous_message = message;
        bool was_saved = saved_credential();
        if (all) {
            clear_text();
        } else if (event->code == 14 && cursor) {
            memmove(password + cursor - 1, password + cursor, length - cursor + 1);
            --cursor;
            password[length--] = 0;
        } else if (event->code == 111 && cursor < length) {
            memmove(password + cursor, password + cursor + 1, length - cursor);
            password[length--] = 0;
        }
        if (text >= 32 && text < 127 && event->code != 14 && event->code != 111) {
            if (length < 63) {
                memmove(password + cursor + 1, password + cursor, length - cursor + 1);
                password[cursor++] = text;
                ++length;
                message = 0;
            } else message = "Password limit: 63 characters";
        } else message = 0;
        /* Changing the saved password turns Forget into Connect, and back. */
        if (was_saved != saved_credential()) page_host_redraw();
        else {
            redraw_password();
            if (previous_message != message) redraw_status();
        }
    }
}

static void name_text(char *out, unsigned capacity, const struct wifi_network *network) {
    unsigned used = 0;
    for (unsigned i = 0; i < network->ssid_length && used + 1 < capacity; ++i) {
        unsigned char c = network->ssid[i];
        if (c >= 32 && c < 127 && c != '\\') out[used++] = (char)c;
        else {
            if (used + 4 >= capacity) break;
            out[used++] = '\\';
            out[used++] = 'x';
            out[used++] = "0123456789abcdef"[c >> 4];
            out[used++] = "0123456789abcdef"[c & 15];
        }
    }
    out[used] = 0;
}

static const char *mode_label(unsigned modes) {
    return modes == ((1u << WIFI_WPA2_PSK) | (1u << WIFI_WPA3_SAE)) ? "WPA2 / WPA3"
           : modes & (1u << WIFI_WPA3_SAE)                          ? "WPA3"
           : modes & (1u << WIFI_WPA2_PSK)                          ? "WPA2"
           : modes == 1                                             ? "Open network"
                                                                    : "Unsupported security";
}

static void button(struct framebuffer *s, unsigned x, unsigned y, unsigned width, const char *text,
                   bool chosen) {
    gfx_rect(s, x, y, width, 44, chosen ? 0x32656d : 0x31515c);
    gfx_text(s, x + 10, y + 15, text, 2, 0xf1f5f6);
}

static void row(struct framebuffer *s, unsigned y, const struct wifi_network *network, bool saved) {
    char name[37];
    name_text(name, sizeof(name), network);
    gfx_rect(s, 16, y, 448, 56, network->modes ? 0x263e49 : 0x222c32);
    gfx_text(s, 26, y + 7, name, 2, 0xf1f5f6);
    gfx_text(s, 26, y + 35, mode_label(network->modes), 1, 0x61e0bd);
    if (saved)
        gfx_text(s, 392, y + 35, wifi_setup_view()->session ? "SESSION" : "SAVED", 1, 0x61e0bd);
}

static void render_password(struct framebuffer *s, unsigned x, unsigned y) {
    if (!visible || !detail) return;
    gfx_rect(s, x, y, 356, 48, all ? 0x32656d : 0xf1f5f6);
    if (cursor < offset) offset = cursor;
    if (cursor > offset + 27) offset = cursor - 27;
    char line[29] = {0};
    for (unsigned i = 0; i < 28 && offset + i < length; ++i)
        line[i] = reveal ? password[offset + i] : '*';
    /* A WPA2 profile imported as a PMK alone has no password to show. */
    bool pmk_only = !length && !editing && saved_selection() && !wifi_setup_saved_password(0);
    gfx_text(s, x + 8, y + 17,
             pmk_only ? (wifi_setup_view()->session ? "Session password" : "Saved password") : line,
             2, all ? 0xf1f5f6 : 0x17232c);
    if (editing && !all) gfx_rect(s, x + 8 + (cursor - offset) * 12, y + 12, 2, 24, 0x17232c);
    wifi_secret_clear(line, sizeof(line));
}

/* A message, else the state; none once connected with an address, which
 * shows instead ("Connected" would say less). */
static const char *status_text(bool address) {
    const struct wifi_setup_view *v = wifi_setup_view();
    if (message) return message;
    if (v->message) return v->message;
    switch (v->radio.state) {
        case WIFI_RADIO_STARTING: return "Starting Wi-Fi...";
        case WIFI_RADIO_SCANNING: return "Scanning...";
        case WIFI_RADIO_CONNECTING: return "Connecting...";
        case WIFI_RADIO_CONNECTED: return address ? 0 : "Getting IP address...";
        case WIFI_RADIO_IDLE:
            return v->has_saved || v->radio.scan_generation ? "Select a network" : "Ready";
        case WIFI_RADIO_FAILED: return v->radio.message ? v->radio.message : "Could not connect";
        default: return "Wi-Fi unavailable";
    }
}

/* Right of the title, right-aligned on one line: the address once
 * connected, with any message before it (a failed attempt that fell back to
 * the saved network, say), otherwise the status. A hardware failure longer
 * than the room keeps its start. */
static void render_status(struct framebuffer *s, unsigned x, unsigned y) {
    if (!visible) return;
    gfx_rect(s, x, y, STATUS_WIDTH, STATUS_HEIGHT, 0x17232c);
    bool address = wifi_setup_view()->radio.state == WIFI_RADIO_CONNECTED && ip_address[0];
    const char *text = status_text(address);
    int right = (int)(x + STATUS_WIDTH);
    if (address) {
        int value = right - gfx_text_width(ip_address, 1);
        gfx_text(s, value - 24, (int)y + 4, "IP:", 1, 0xb9c9d0);
        gfx_text(s, value, (int)y + 4, ip_address, 1, 0xf1f5f6);
        right = value - 24 - 12;
    }
    if (!text || right <= (int)x) return;
    char line[STATUS_WIDTH / 6 + 1];
    unsigned n = (unsigned)strlen(text), room = (unsigned)(right - (int)x) / 6;
    if (n > room) n = room;
    memcpy(line, text, n);
    line[n] = 0;
    gfx_text(s, right - gfx_text_width(line, 1), (int)y + 4, line, 1, 0xf1f5f6);
}

void desktop_wifi_network_status(const char *address) {
    char next[16] = {0};
    unsigned n = 0;
    if (address)
        while (n < sizeof(next) - 1 &&
               ((address[n] >= '0' && address[n] <= '9') || address[n] == '.')) {
            next[n] = address[n];
            ++n;
        }
    if (address && address[n]) next[0] = 0;
    if (strcmp(next, ip_address)) {
        memcpy(ip_address, next, sizeof(next));
        if (visible) redraw_status();
    }
}

void desktop_wifi_render(struct framebuffer *s) {
    if (!visible) return;
    const struct wifi_setup_view *v = wifi_setup_view();
    gfx_rect(s, 0, 48, 480, 752, 0x17232c);
    gfx_text(s, 20, 68, "WI-FI", 2, 0xf1f5f6);
    if (!detail) {
        button(s, 16, 104, 288, "SCAN NETWORKS", false);
        button(s, 320, 104, 144, "BACK", false);
        if (v->has_saved) row(s, 164, &v->saved, true);
        for (unsigned i = 0; i < ROWS && page * ROWS + i < v->radio.count; ++i)
            row(s, list_top() + i * 64, &v->radio.networks[page * ROWS + i], false);
        /* The status says "Scanning..."; only a finished scan's empty result shows here. */
        if (no_networks(v)) gfx_text(s, 20, list_top() + 22, "NO NETWORKS FOUND", 1, 0xf1f5f6);
        if (page) button(s, 16, paging_y(), 216, "PREVIOUS", false);
        if ((page + 1) * ROWS < v->radio.count) button(s, 248, paging_y(), 216, "NEXT", false);
        drawn = (struct drawn_screen){.valid = true,
                                      .saved = v->has_saved,
                                      .saved_network = v->saved,
                                      .top = list_top(),
                                      .previous = page != 0,
                                      .next = (page + 1) * ROWS < v->radio.count,
                                      .paging_y = paging_y()};
        for (unsigned i = 0; i < ROWS && page * ROWS + i < v->radio.count; ++i)
            drawn.row[drawn.rows++] = v->radio.networks[page * ROWS + i];
    } else {
        char name[129];
        name_text(name, sizeof(name), &selected);
        char first[73] = {0};
        unsigned n = (unsigned)strlen(name);
        memcpy(first, name, n < 72 ? n : 72);
        gfx_text(s, 20, 102, first, 1, 0xf1f5f6);
        if (n > 72) gfx_text(s, 20, 118, name + 72, 1, 0xf1f5f6);
        const char *forget = v->session ? "FORGET NETWORK" : "FORGET SAVED NETWORK";
        drawn = (struct drawn_screen){.valid = true, .detail = true, .forget = saved_credential()};
        button(s, 16, DETAIL_ACTIONS_Y, 288, drawn.forget ? forget : "CONNECT", false);
        button(s, 320, DETAIL_ACTIONS_Y, 144, "BACK", false);
        if (security != WIFI_OPEN) {
            render_password(s, 16, DETAIL_PASSWORD_Y);
            button(s, 380, DETAIL_PASSWORD_Y + 2, 84, reveal ? "HIDE" : "SHOW", false);
        } else gfx_text(s, 20, DETAIL_PASSWORD_Y + 12, "OPEN NETWORK / NO PASSWORD", 1, 0xf1f5f6);
        if (selected.modes & (1u << WIFI_WPA2_PSK))
            button(s, 16, DETAIL_SECURITY_Y, 216, "WPA2", security == WIFI_WPA2_PSK);
        if (selected.modes & (1u << WIFI_WPA3_SAE))
            button(s, 248, DETAIL_SECURITY_Y, 216, "WPA3", security == WIFI_WPA3_SAE);
    }
    render_status(s, STATUS_X, STATUS_Y);
}
