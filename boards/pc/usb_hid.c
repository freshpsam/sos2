#include "pc.h"
#include "platform.h"

/* USB keyboards and pointers (usb_hid.c). A keyboard reports in the boot
 * protocol: eight bytes, the modifiers and up to six keys held, turned into
 * Linux key codes (the HID usage tables' keyboard page) with
 * kernel/input.c's modifiers, Caps Lock and repeat. A pointer (a mouse, or
 * QEMU's tablet) reports as its report descriptor says, which a small parser
 * reads for its first button, X, Y and wheel: relative motion moves a
 * position kept on the desktop's part of the monitor, absolute positions
 * span the monitor and stop at the desktop's edges.
 * https://www.usb.org/hid (Device Class Definition for HID 1.11: 6.2.2 and
 * appendix B; HID Usage Tables 1.4: 10, the keyboard page) */
#define KEYBOARDS 4
#define POINTERS  4
#define KEY_QUEUE 64
#define SAMPLES   64
#define REPORT    64

struct keyboard {
    struct usb_endpoint in;
    uint8_t report[8] __attribute__((aligned(8)));
    uint8_t last[8];
    bool used;
};

struct field {
    uint16_t offset;
    uint8_t size;
    bool present, relative;
    int32_t minimum, maximum;
};

struct pointer {
    struct usb_endpoint in;
    uint8_t report[REPORT] __attribute__((aligned(REPORT)));
    struct field button, x, y, wheel;
    uint8_t id;
    bool ids, used;
};

static struct keyboard keyboards[KEYBOARDS];
static struct pointer pointers[POINTERS];
static uint8_t report_descriptor[512] __attribute__((aligned(512)));

static struct {
    uint8_t code;
    bool down;
} key_queue[KEY_QUEUE];

static unsigned key_head, key_tail;
static bool key_reset;
static struct pc_pointer samples[SAMPLES];
static unsigned sample_head, sample_tail;
static int32_t pointer_x, pointer_y;
static bool pointer_down;
static uint32_t monitor_width = 1, monitor_height = 1, area_left, area_top, area_width = 1,
                area_height = 1;

/* The keyboard page's usages 0x04 to 0x65 as Linux key codes; 0 has none. */
static const uint8_t usage_codes[0x66] = {
    [0x04] = 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
    [0x1e] = 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    [0x28] = 28, 1, 14, 15, 57, 12, 13, 26, 27, 43, 43, 39, 40, 41, 51, 52, 53, 58,
    [0x3a] = 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88,
    [0x46] = 99, 70, 119, 110, 102, 104, 111, 107, 109, 106, 105, 108, 103,
    [0x53] = 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71, 72, 73, 82, 83, 86, 127};
/* The modifier bits, left control first. */
static const uint8_t modifier_codes[8] = {29, 42, 56, 125, 97, 54, 100, 126};

static void queue_key(uint8_t code, bool down) {
    if (!code) return;
    if ((key_tail + 1) % KEY_QUEUE == key_head) {
        key_reset = true; /* lost: every key released */
        return;
    }
    key_queue[key_tail].code = code;
    key_queue[key_tail].down = down;
    key_tail = (key_tail + 1) % KEY_QUEUE;
}

static bool holds(const uint8_t *report, uint8_t usage) {
    for (unsigned i = 2; i < 8; ++i)
        if (report[i] == usage) return true;
    return false;
}

static void keyboard_report(struct keyboard *k, const uint8_t *report, uint32_t length) {
    if (length < 8 || report[2] == 1) return; /* short, or too many keys held */
    for (unsigned bit = 0; bit < 8; ++bit)
        if ((report[0] ^ k->last[0]) & (1u << bit))
            queue_key(modifier_codes[bit], report[0] & (1u << bit));
    for (unsigned i = 2; i < 8; ++i) {
        uint8_t before = k->last[i], now = report[i];
        if (before >= 4 && before < sizeof(usage_codes) && !holds(report, before))
            queue_key(usage_codes[before], false);
        if (now >= 4 && now < sizeof(usage_codes) && !holds(k->last, now))
            queue_key(usage_codes[now], true);
    }
    memcpy(k->last, report, 8);
}

static uint32_t bits(const uint8_t *data, uint32_t length, unsigned offset, unsigned size) {
    uint32_t value = 0;
    for (unsigned i = 0; i < size && i < 32; ++i) {
        unsigned bit = offset + i;
        if (bit / 8 < length && (data[bit / 8] >> (bit % 8) & 1)) value |= 1u << i;
    }
    return value;
}

static int32_t field_value(const struct field *f, const uint8_t *data, uint32_t length) {
    uint32_t value = bits(data, length, f->offset, f->size);
    if ((f->minimum < 0 || f->relative) && f->size && f->size < 32 && value >> (f->size - 1))
        return (int32_t)(value | ~0u << f->size);
    return (int32_t)value;
}

static int32_t clamp(int64_t value, uint32_t start, uint32_t extent) {
    return value < start                      ? (int32_t)start
           : value >= (int64_t)start + extent ? (int32_t)(start + extent - 1)
                                              : (int32_t)value;
}

static void push_sample(int32_t wheel) {
    struct pc_pointer sample = {pointer_x, pointer_y, wheel, pointer_down, true};
    unsigned next = (sample_tail + 1) % SAMPLES;
    if (next == sample_head) {
        /* Full: the newest absorbs this one unless a button changes. */
        struct pc_pointer *last = &samples[(sample_tail + SAMPLES - 1) % SAMPLES];
        if (last->down == sample.down) {
            sample.wheel += last->wheel;
            *last = sample;
        }
        return;
    }
    samples[sample_tail] = sample;
    sample_tail = next;
}

static void pointer_report(struct pointer *p, const uint8_t *report, uint32_t length) {
    if (p->ids) {
        if (!length || report[0] != p->id) return;
        ++report;
        --length;
    }
    if (p->x.relative) {
        pointer_x =
            clamp((int64_t)pointer_x + field_value(&p->x, report, length), area_left, area_width);
        pointer_y =
            clamp((int64_t)pointer_y + field_value(&p->y, report, length), area_top, area_height);
    } else {
        int64_t span_x = (int64_t)p->x.maximum - p->x.minimum + 1,
                span_y = (int64_t)p->y.maximum - p->y.minimum + 1;
        if (span_x > 0)
            pointer_x = clamp(((int64_t)field_value(&p->x, report, length) - p->x.minimum) *
                                  monitor_width / span_x,
                              area_left, area_width);
        if (span_y > 0)
            pointer_y = clamp(((int64_t)field_value(&p->y, report, length) - p->y.minimum) *
                                  monitor_height / span_y,
                              area_top, area_height);
    }
    pointer_down = p->button.present && bits(report, length, p->button.offset, 1);
    push_sample(p->wheel.present ? field_value(&p->wheel, report, length) : 0);
}

void usb_hid_report(unsigned slot, uint8_t dci, const uint8_t *data, uint32_t length) {
    for (unsigned i = 0; i < KEYBOARDS; ++i)
        if (keyboards[i].used && keyboards[i].in.slot == slot && keyboards[i].in.dci == dci)
            keyboard_report(&keyboards[i], data, length);
    for (unsigned i = 0; i < POINTERS; ++i)
        if (pointers[i].used && pointers[i].in.slot == slot && pointers[i].in.dci == dci)
            pointer_report(&pointers[i], data, length);
}

/* A report descriptor's first pointer: its report, first button, X, Y and
 * wheel. Short items only; usages without a page take the current one. */
static bool parse_report(const uint8_t *d, unsigned n, struct pointer *p) {
    uint32_t page = 0, size = 0, count = 0, usages[16], usage_minimum = 0, usage_maximum = 0;
    int32_t minimum = 0, maximum = 0;
    unsigned usage_count = 0, id = 0;
    bool range = false;
    static uint16_t offsets[256];
    memset(offsets, 0, sizeof(offsets));
    for (unsigned at = 0; at < n;) {
        uint8_t prefix = d[at++];
        if (prefix == 0xfe) { /* a long item */
            if (at >= n) break;
            at += 2u + d[at];
            continue;
        }
        unsigned length = prefix & 3 ? 1u << ((prefix & 3) - 1) : 0, type = (prefix >> 2) & 3,
                 tag = prefix >> 4;
        if (at + length > n) break;
        uint32_t value = 0;
        for (unsigned i = 0; i < length; ++i) value |= (uint32_t)d[at + i] << (8 * i);
        int32_t signed_value = length && length < 4 && value >> (8 * length - 1)
                                   ? (int32_t)(value | ~0u << (8 * length))
                                   : (int32_t)value;
        at += length;
        if (type == 1) {
            if (tag == 0) page = value;
            else if (tag == 1) minimum = signed_value;
            else if (tag == 2) maximum = minimum >= 0 ? (int32_t)value : signed_value;
            else if (tag == 7) size = value;
            else if (tag == 8 && value < 256) {
                id = value;
                p->ids = true;
            } else if (tag == 9) count = value;
        } else if (type == 2) {
            uint32_t usage = length == 4 ? value : page << 16 | value;
            if (tag == 0 && usage_count < 16) usages[usage_count++] = usage;
            else if (tag == 1) {
                usage_minimum = usage;
                range = true;
            } else if (tag == 2) usage_maximum = usage;
        } else if (type == 0) {
            if (tag == 8 && size && size <= 32 && count <= 64) {
                bool constant = value & 1, variable = value & 2, relative = value & 4;
                for (unsigned f = 0; f < count; ++f) {
                    uint32_t usage = range ? (usage_minimum + f <= usage_maximum ? usage_minimum + f
                                                                                 : usage_maximum)
                                     : usage_count ? usages[f < usage_count ? f : usage_count - 1]
                                                   : 0;
                    struct field field = {(uint16_t)(offsets[id] + f * size),
                                          (uint8_t)size,
                                          true,
                                          relative,
                                          minimum,
                                          maximum};
                    struct field *target = 0;
                    if (constant || !variable) continue;
                    if (usage == 0x00090001) target = &p->button;
                    else if (usage == 0x00010030) target = &p->x;
                    else if (usage == 0x00010031) target = &p->y;
                    else if (usage == 0x00010038) target = &p->wheel;
                    if (target && !target->present && (!p->x.present || p->id == id)) {
                        *target = field;
                        p->id = (uint8_t)id;
                    }
                }
                offsets[id] = (uint16_t)(offsets[id] + size * count);
            }
            usage_count = 0;
            range = false;
        }
    }
    return p->x.present && p->y.present && p->x.relative == p->y.relative;
}

bool usb_hid_attach(unsigned slot, unsigned interface, uint8_t protocol,
                    const struct usb_endpoint *in, uint16_t report_length) {
    /* No repeats while nothing changes (SET_IDLE 0), which some refuse. */
    (void)usb_control(slot, 0x21, 0x0a, 0, (uint16_t)interface, 0, 0, 200000);
    if (protocol == 1) {
        for (unsigned i = 0; i < KEYBOARDS; ++i) {
            struct keyboard *k = &keyboards[i];
            if (k->used) continue;
            if (!usb_control(slot, 0x21, 0x0b, 0, (uint16_t)interface, 0, 0, 200000)) return false;
            *k = (struct keyboard){.in = *in, .used = true};
            return usb_interrupt_in(&k->in, k->report, sizeof(k->report)) || (k->used = false);
        }
        return false;
    }
    if (!report_length || report_length > sizeof(report_descriptor)) return false;
    for (unsigned i = 0; i < POINTERS; ++i) {
        struct pointer *p = &pointers[i];
        if (p->used) continue;
        *p = (struct pointer){.in = *in};
        if (!usb_control(slot, 0x81, 6, 0x2200, (uint16_t)interface, report_descriptor,
                         report_length, 200000) ||
            !parse_report(report_descriptor, report_length, p))
            return false;
        /* The report protocol, which the descriptor describes. */
        (void)usb_control(slot, 0x21, 0x0b, 1, (uint16_t)interface, 0, 0, 200000);
        p->used = true;
        uint32_t length = in->max_packet && in->max_packet < REPORT ? in->max_packet : REPORT;
        return usb_interrupt_in(&p->in, p->report, length) || (p->used = false);
    }
    return false;
}

void usb_detach(unsigned slot) {
    for (unsigned i = 0; i < KEYBOARDS; ++i)
        if (keyboards[i].used && keyboards[i].in.slot == slot) {
            keyboards[i].used = false;
            key_reset = true;
        }
    for (unsigned i = 0; i < POINTERS; ++i)
        if (pointers[i].used && pointers[i].in.slot == slot) {
            pointers[i].used = false;
            if (pointer_down) {
                pointer_down = false;
                push_sample(0);
            }
        }
    usb_storage_detach(slot);
}

void usb_input_screen(uint32_t monitor_w, uint32_t monitor_h, uint32_t left, uint32_t top,
                      uint32_t width, uint32_t height) {
    monitor_width = monitor_w ? monitor_w : 1;
    monitor_height = monitor_h ? monitor_h : 1;
    area_left = left;
    area_top = top;
    area_width = width ? width : 1;
    area_height = height ? height : 1;
    pointer_x = (int32_t)(area_left + area_width / 2);
    pointer_y = (int32_t)(area_top + area_height / 2);
}

bool usb_pointer_poll(struct pc_pointer *pointer) {
    if (sample_head == sample_tail) return false;
    *pointer = samples[sample_head];
    sample_head = (sample_head + 1) % SAMPLES;
    return true;
}

/* As kernel/input.c's keyboard: modifiers from the keys held, Caps Lock
 * toggled on its press, and the last key repeated after 450 ms every 40. */
static struct {
    bool pressed[256], caps;
    unsigned repeat_key;
    uint64_t repeat_at;
} keys;

static unsigned modifiers(void) {
    return ((keys.pressed[42] || keys.pressed[54]) ? INPUT_SHIFT : 0) |
           ((keys.pressed[29] || keys.pressed[97]) ? INPUT_CTRL : 0) |
           ((keys.pressed[56] || keys.pressed[100]) ? INPUT_ALT : 0) |
           ((keys.pressed[125] || keys.pressed[126]) ? INPUT_META : 0) |
           (keys.caps ? INPUT_CAPS_LOCK : 0);
}

static bool repeatable(unsigned key) {
    return key != 42 && key != 54 && key != 29 && key != 97 && key != 56 && key != 100 &&
           key != 125 && key != 126 && key != 58 && key != 69 && key != 70;
}

static bool key_transition(struct user_input_event *event, unsigned code, unsigned value) {
    bool was = keys.pressed[code], down = value != 0, repeat = down && was;
    if ((!down && !was) || (value == 2 && !was) || (repeat && !repeatable(code))) return false;
    keys.pressed[code] = down;
    if (code == 58 && down && !was) keys.caps = !keys.caps;
    if (down && repeatable(code)) {
        keys.repeat_key = code;
        keys.repeat_at = time_us() + (repeat ? 40000 : 450000);
    } else if (!down && keys.repeat_key == code) keys.repeat_key = 0;
    *event =
        (struct user_input_event){.type = INPUT_KEY,
                                  .flags = (down ? INPUT_DOWN : 0) | (repeat ? INPUT_REPEAT : 0),
                                  .modifiers = modifiers(),
                                  .code = code};
    return true;
}

bool usb_keyboard_poll(struct user_input_event *event) {
    if (key_reset) {
        key_reset = false;
        key_head = key_tail = 0;
        memset(&keys, 0, sizeof(keys));
        *event = (struct user_input_event){.type = INPUT_RESET};
        return true;
    }
    while (key_head != key_tail) {
        unsigned code = key_queue[key_head].code, down = key_queue[key_head].down;
        key_head = (key_head + 1) % KEY_QUEUE;
        if (key_transition(event, code, down)) return true;
    }
    if (keys.repeat_key && time_us() >= keys.repeat_at)
        return key_transition(event, keys.repeat_key, 2);
    return false;
}
