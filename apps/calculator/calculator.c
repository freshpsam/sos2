/* Calculator: a native SOS2 app (include/apps.h). It draws with the desktop's
 * 5x7 font (kernel/graphics.c) into its 480x752 page surface and takes touch
 * and keyboard input from the page host. Multiplication and division come
 * before addition and subtraction: 12+3*4= is 24. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sos2/services.h>
#include "graphics.h"

enum {
    WIDTH = WEB_WIDTH,
    HEIGHT = WEB_HEIGHT,
    DISPLAY_X = 16,
    DISPLAY_Y = 16,
    DISPLAY_W = 448,
    DISPLAY_H = 180,
    KEYS_X = 16,
    KEYS_Y = 212,
    KEY_W = 104,
    KEY_H = 98,
    KEY_GAP = 10,
    COLUMNS = 4,
    ROWS = 5,
    NUMBERS = 16,
    ENTRY_DIGITS = 15,
};

enum {
    BACKGROUND = 0x17232c,
    DISPLAY = 0x0f181d,
    TEXT = 0xf1f5f6,
    MUTED = 0x8fa4ad,
    DIGIT = 0x31515c,
    FUNCTION = 0x253b44,
    OPERATOR = 0x2b6b6e,
    EQUALS = 0x61e0bd,
};

static uint32_t pixels[WIDTH * HEIGHT];
/* ARGB so that every pixel is opaque. */
static struct framebuffer screen = {pixels, WIDTH, HEIGHT, WIDTH * 4, PIXEL_ARGB8888};

/* Labels: '*' and '/' draw as multiply and divide signs, '<' as backspace
 * and '~' as plus-minus. Each key's action is the same character. */
static const struct key {
    char label;
    uint32_t color;
} keys[ROWS][COLUMNS] = {
    {{'C', FUNCTION}, {'<', FUNCTION}, {'%', FUNCTION}, {'/', OPERATOR}},
    {{'7', DIGIT}, {'8', DIGIT}, {'9', DIGIT}, {'*', OPERATOR}},
    {{'4', DIGIT}, {'5', DIGIT}, {'6', DIGIT}, {'-', OPERATOR}},
    {{'1', DIGIT}, {'2', DIGIT}, {'3', DIGIT}, {'+', OPERATOR}},
    {{'~', FUNCTION}, {'0', DIGIT}, {'.', DIGIT}, {'=', EQUALS}},
};

/* The expression so far is numbers[0] ops[0] ... numbers[count-1] ops[count-1],
 * followed by the operand: typed text in entry, or a computed value. */
static double numbers[NUMBERS];
static char ops[NUMBERS];
static unsigned count;
static char entry[24];
static double operand;
static bool has_operand, result, error;

static void glyph(int x, int y, const uint8_t rows[7], unsigned scale, uint32_t color) {
    for (unsigned row = 0; row < 7; ++row)
        for (unsigned column = 0; column < 5; ++column)
            if (rows[row] & (1u << (4 - column)))
                gfx_rect(&screen, x + (int)(column * scale), y + (int)(row * scale), (int)scale,
                         (int)scale, color);
}

static void text(int x, int y, const char *s, unsigned scale, uint32_t color) {
    static const uint8_t times[7] = {0, 17, 10, 4, 10, 17, 0}, divide[7] = {0, 4, 0, 31, 0, 4, 0},
                         backspace[7] = {0, 4, 8, 31, 8, 4, 0},
                         plus_minus[7] = {4, 4, 31, 4, 4, 0, 31}, minus[7] = {0, 0, 0, 31, 0, 0, 0};
    for (; *s; ++s, x += 6 * (int)scale) {
        const uint8_t *special = *s == '*'   ? times
                                 : *s == '/' ? divide
                                 : *s == '<' ? backspace
                                 : *s == '~' ? plus_minus
                                 : *s == '-' ? minus
                                             : 0;
        if (special) glyph(x, y, special, scale, color);
        else {
            char one[2] = {*s, 0};
            gfx_text(&screen, x, y, one, scale, color);
        }
    }
}

static void format_number(double value, char *out, size_t size) {
    if (value == 0) value = 0; /* No "-0". */
    snprintf(out, size, "%.12g", value);
    if (strlen(out) > 16) snprintf(out, size, "%.9g", value);
}

static void present(int x, int y, int width, int height) {
    (void)sos2_present_damage(pixels, (unsigned)x, (unsigned)y, (unsigned)width, (unsigned)height);
}

/* The main line: the operand being typed, the computed one or the last number. */
static void display_text(char *out, size_t size) {
    if (error) snprintf(out, size, "Error");
    else if (entry[0]) snprintf(out, size, "%s", entry);
    else if (has_operand) format_number(operand, out, size);
    else if (count) format_number(numbers[count - 1], out, size);
    else snprintf(out, size, "0");
}

/* The expression line: the numbers and operators so far, then the number
 * being entered, typed or made by % or the sign key. A result is not echoed. */
static void expression_text(char *out, size_t size) {
    char number[32];
    out[0] = 0;
    for (unsigned i = 0; i < count; ++i) {
        format_number(numbers[i], number, sizeof(number));
        size_t used = strlen(out);
        snprintf(out + used, size - used, "%s%s %c", i ? " " : "", number, ops[i]);
    }
    const char *input = 0;
    if (error) return;
    if (entry[0]) input = entry;
    else if (has_operand && !result) {
        format_number(operand, number, sizeof(number));
        input = number;
    }
    if (input) {
        size_t used = strlen(out);
        snprintf(out + used, size - used, "%s%s", count ? " " : "", input);
    }
}

static void render_display(void) {
    gfx_rect(&screen, DISPLAY_X, DISPLAY_Y, DISPLAY_W, DISPLAY_H, DISPLAY);
    /* The newest 23 characters fit the line. */
    char line[NUMBERS * 24 + 32];
    expression_text(line, sizeof(line));
    size_t length = strlen(line);
    const char *expression = length > 23 ? line + length - 23 : line;
    text(DISPLAY_X + DISPLAY_W - 16 - gfx_text_width(expression, 3), DISPLAY_Y + 20, expression, 3,
         MUTED);
    char shown[32];
    display_text(shown, sizeof(shown));
    unsigned scale = 7;
    while (scale > 3 && gfx_text_width(shown, scale) > DISPLAY_W - 32) --scale;
    text(DISPLAY_X + DISPLAY_W - 16 - gfx_text_width(shown, scale),
         DISPLAY_Y + DISPLAY_H - 20 - 7 * (int)scale, shown, scale, TEXT);
}

static uint32_t lighter(uint32_t color) {
    uint32_t out = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        uint32_t channel = color >> shift & 255;
        out |= (channel + (255 - channel) / 4) << shift;
    }
    return out;
}

static void key_rect(int index, int *x, int *y) {
    *x = KEYS_X + index % COLUMNS * (KEY_W + KEY_GAP);
    *y = KEYS_Y + index / COLUMNS * (KEY_H + KEY_GAP);
}

static void render_key(int index, bool pressed) {
    const struct key *key = &keys[index / COLUMNS][index % COLUMNS];
    int x, y;
    key_rect(index, &x, &y);
    gfx_rect(&screen, x, y, KEY_W, KEY_H, pressed ? lighter(key->color) : key->color);
    char label[2] = {key->label, 0};
    text(x + (KEY_W - gfx_text_width(label, 5)) / 2, y + (KEY_H - 35) / 2, label, 5,
         key->color == EQUALS ? BACKGROUND : TEXT);
}

static int key_at(int x, int y) {
    if (x < KEYS_X || y < KEYS_Y) return -1;
    int column = (x - KEYS_X) / (KEY_W + KEY_GAP), row = (y - KEYS_Y) / (KEY_H + KEY_GAP);
    if (column >= COLUMNS || row >= ROWS || (x - KEYS_X) % (KEY_W + KEY_GAP) >= KEY_W ||
        (y - KEYS_Y) % (KEY_H + KEY_GAP) >= KEY_H)
        return -1;
    return row * COLUMNS + column;
}

static void clear(void) {
    count = 0;
    entry[0] = 0;
    operand = 0;
    has_operand = result = error = false;
}

/* The current operand's value: typed, computed, or the last number. */
static double current(void) {
    if (entry[0]) return strtod(entry, 0);
    if (has_operand) return operand;
    return count ? numbers[count - 1] : 0;
}

static bool evaluate(double last, double *out) {
    double total = 0, term = count ? numbers[0] : last;
    char pending = '+';
    for (unsigned i = 0; i < count; ++i) {
        double value = i + 1 < count ? numbers[i + 1] : last;
        if (ops[i] == '*') term *= value;
        else if (ops[i] == '/') {
            if (value == 0) return false;
            term /= value;
        } else {
            total = pending == '+' ? total + term : total - term;
            pending = ops[i];
            term = value;
        }
    }
    total = pending == '+' ? total + term : total - term;
    *out = total;
    return isfinite(total);
}

static void type(char c) {
    if (error || result) clear();
    if (has_operand) has_operand = false; /* A computed operand is replaced. */
    size_t length = strlen(entry);
    unsigned digits = 0;
    for (size_t i = 0; i < length; ++i)
        if (entry[i] >= '0' && entry[i] <= '9') ++digits;
    if (c == '.') {
        if (strchr(entry, '.')) return;
        if (!length || !strcmp(entry, "-")) {
            strcat(entry, "0.");
            return;
        }
    } else if (digits >= ENTRY_DIGITS) return;
    else if (!strcmp(entry, "0") || !strcmp(entry, "-0")) {
        entry[length - 1] = c;
        return;
    }
    entry[length] = c;
    entry[length + 1] = 0;
}

static void push_operator(char op) {
    if (error) return;
    if (!entry[0] && !has_operand && count) {
        ops[count - 1] = op;
        return;
    } /* Replace it. */
    if (count == NUMBERS) return;
    numbers[count] = current();
    ops[count++] = op;
    entry[0] = 0;
    has_operand = result = false;
}

static void equals(void) {
    if (error) return;
    double value = current();
    if (!evaluate(value, &value)) {
        clear();
        error = true;
        return;
    }
    count = 0;
    entry[0] = 0;
    operand = value;
    has_operand = result = true;
}

static void act(char c) {
    if ((c >= '0' && c <= '9') || c == '.') type(c);
    else if (c == '+' || c == '-' || c == '*' || c == '/') push_operator(c);
    else if (c == '=') equals();
    else if (c == 'C') clear();
    else if (c == '<') {
        if (error) clear();
        else if (entry[0]) {
            entry[strlen(entry) - 1] = 0;
            if (!strcmp(entry, "-")) entry[0] = 0;
        }
    } else if (c == '~') {
        if (error) return;
        if (entry[0]) {
            if (entry[0] == '-') memmove(entry, entry + 1, strlen(entry));
            else {
                memmove(entry + 1, entry, strlen(entry) + 1);
                entry[0] = '-';
            }
        } else if (has_operand) {
            operand = -operand;
            result = false;
        } else strcpy(entry, "-0");
    } else if (c == '%') {
        if (error) return;
        operand = current() / 100;
        entry[0] = 0;
        has_operand = true;
        result = false;
    } else return;
    render_display();
    present(DISPLAY_X, DISPLAY_Y, DISPLAY_W, DISPLAY_H);
}

static int pressed = -1, armed = -1;

static void show_pressed(int index) {
    if (index == pressed) return;
    int x, y;
    if (pressed >= 0) {
        render_key(pressed, false);
        key_rect(pressed, &x, &y);
        present(x, y, KEY_W, KEY_H);
    }
    pressed = index;
    if (pressed >= 0) {
        render_key(pressed, true);
        key_rect(pressed, &x, &y);
        present(x, y, KEY_W, KEY_H);
    }
}

static void pointer(const struct user_input_event *event) {
    int hit = key_at(event->x, event->y);
    if (event->flags & INPUT_CANCEL) {
        armed = -1;
        show_pressed(-1);
        return;
    }
    if (event->code == 1) {
        armed = hit;
        show_pressed(hit); /* drawn first: a syscall waits a loop pass */
        if (hit >= 0) (void)sos2_haptic_tap();
        return;
    }
    if (event->code == 2) {
        int key = armed;
        armed = -1;
        show_pressed(-1);
        if (key >= 0 && key == hit) act(keys[key / COLUMNS][key % COLUMNS].label);
        return;
    }
    if (event->flags & INPUT_DOWN) show_pressed(hit == armed ? armed : -1);
}

/* US layout, main keys and keypad. */
static char key_action(unsigned code, bool shift) {
    if (code >= 2 && code <= 10)
        return shift ? (code == 9 ? '*' : code == 6 ? '%' : 0) : (char)('1' + code - 2);
    switch (code) {
        case 11: return shift ? 0 : '0';
        case 12:
        case 74: return '-';
        case 13: return shift ? '+' : '=';
        case 78: return '+';
        case 55: return '*';
        case 53:
        case 98: return '/';
        case 52:
        case 83: return '.';
        case 28:
        case 96: return '=';
        case 14: return '<';
        case 1:
        case 46:
        case 111: return 'C';
        case 71: return '7';
        case 72: return '8';
        case 73: return '9';
        case 75: return '4';
        case 76: return '5';
        case 77: return '6';
        case 79: return '1';
        case 80: return '2';
        case 81: return '3';
        case 82: return '0';
        default: return 0;
    }
}

static void handle(const struct user_input_event *event) {
    if (event->type == INPUT_POINTER) pointer(event);
    else if (event->type == INPUT_KEY && (event->flags & INPUT_DOWN)) {
        char c = key_action(event->code, event->modifiers & INPUT_SHIFT);
        if (c && (!(event->flags & INPUT_REPEAT) || c == '<')) act(c);
    } else if (event->type == INPUT_RESET || event->type == INPUT_FOCUS) {
        armed = -1;
        show_pressed(-1);
    }
}

int main(void) {
    gfx_rect(&screen, 0, 0, WIDTH, HEIGHT, BACKGROUND);
    render_display();
    for (int i = 0; i < ROWS * COLUMNS; ++i) render_key(i, false);
    sos2_present(pixels, sizeof(pixels));
    for (;;) {
        struct user_input_event event;
        if (sos2_next_event(&event) == 1) {
            handle(&event);
            continue;
        }
        struct timespec pause = {0, 16 * 1000 * 1000};
        nanosleep(&pause, 0);
    }
}
