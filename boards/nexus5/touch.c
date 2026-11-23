#include "nexus5.h"
#include "graphics.h"
#include "platform.h"
#include "qup_i2c.h"

/* The touch screen: a Synaptics S3350 (product ID PLG218) at I2C address
 * 0x70 on BLSP1 QUP2 (GPIO6 SDA, GPIO7 SCL), its attention line on GPIO5
 * (low while an interrupt it enabled is pending) and its reset on GPIO8, as
 * Google's msm8974-hammerhead-input.dtsi and Linux's
 * qcom-msm8974-lge-nexus5-hammerhead.dts describe it; its supplies (PM8941
 * L22 and LVS3) are the display's too, on from the bootloader.
 * It speaks RMI4 (Linux drivers/input/rmi4: rmi_driver.c, rmi_i2c.c,
 * rmi_f01.c, rmi_f12.c): registers in 256-byte pages chosen by writing
 * 0xff, and a page description table (PDT) from 0xe9 down in each page,
 * 6-byte entries naming each function's query, command, control and data
 * registers. F01 is the device itself: its status, then the interrupt
 * status bits (reading them clears them and the attention line), its
 * control (sleep mode, the configured flag) and the interrupt enables.
 * F12 is the 2D sensor: its registers are described by its queries (a
 * presence map and each register's size and subpackets), Data1 holding
 * 8 bytes per object (type, X, Y, Z, widths) and Control8 the sensor's
 * largest X and Y. Interrupt bits follow the functions in PDT order.
 * Polled: the attention line is read, it raises no interrupt. */

enum {
    PIN_PULL_UP = 3, /* pull in bits 1:0, drive (mA / 2 - 1) in 8:6 */
    PIN_OUTPUT = 1 << 9,
    PIN_IN = 1,
    PIN_OUT = 2,
    ATTENTION = 5,
    SDA = 6,
    SCL = 7,
    RESET = 8,
};

enum {
    ADDRESS = 0x70,
    PAGE_SELECT = 0xff,
    PDT_PROPERTIES = 0xef,
    PDT_TOP = 0xe9,
    PDT_BOTTOM = 0x05,
    PDT_ENTRY = 6,
    PAGES = 16,
    F01 = 0x01,
    F12 = 0x12,
    F01_UNCONFIGURED = 0x80,
    F01_BOOTLOADER = 0x40,
    F01_SLEEP_MASK = 3,
    F01_SENSOR_SLEEP = 1,
    F01_NOSLEEP = 1 << 2,
    F01_CONFIGURED = 1 << 7,
    IRQ_BYTES = 4,
    OBJECT_BYTES = 8,
    OBJECTS = 5, /* read: the first finger down is followed, in its slot */
    OBJECT_NONE = 0,
    OBJECT_FINGER = 1,
    OBJECT_STYLUS = 2,
    OBJECT_UNCLASSIFIED = 4,
    OBJECT_GLOVED = 6,
    REGISTERS = 64,
    RESET_US = 10000,
    START_US = 1000000,
    ANSWER_POLL_US = 50000,
    RETRY_US = 2000,
    FAILURES = 3,
    NO_SLOT = 0xff,
};

struct function {
    uint16_t query, control, data;
    unsigned irq; /* its first interrupt bit */
    bool found;
};

/* An F12 register descriptor: the registers present, ascending. */
struct descriptor {
    unsigned count;
    uint8_t reg[REGISTERS];
    uint16_t size[REGISTERS];
    bool subpacket0[REGISTERS];
};

/* At 355 kHz, as Linux's hammerhead device tree runs it. */
static struct qup_i2c bus = {.base = 0xf9924000, .khz = 355};

static struct touch_state {
    bool ready, asleep, down, waking; /* waking: a wake the controller missed */
    struct function f01, f12;
    unsigned irq_bytes, page;
    unsigned data1_offset, objects; /* Data1's place in F12's data, and its size */
    uint32_t max_x, max_y, width, height;
    uint8_t slot, failures;
    uint32_t x, y;
    uint64_t retry_after;
} touch;

static bool select_page(unsigned page) {
    if (page == touch.page) return true;
    if (!qup_i2c_write(&bus, ADDRESS, PAGE_SELECT, (uint8_t)page)) {
        touch.page = ~0u;
        return false;
    }
    touch.page = page;
    return true;
}

static bool rmi_read(uint16_t address, uint8_t *data, size_t length) {
    return select_page(address >> 8) && qup_i2c_read(&bus, ADDRESS, address & 0xff, data, length);
}

static bool rmi_write(uint16_t address, uint8_t value) {
    return select_page(address >> 8) && qup_i2c_write(&bus, ADDRESS, address & 0xff, value);
}

static bool answers(void) {
    uint8_t properties;
    touch.page = ~0u;
    return rmi_read(PDT_PROPERTIES, &properties, 1);
}

/* Each page's table, top down, until two pages in a row are empty: F01's
 * and F12's registers and interrupt bits, and how many bytes the interrupt
 * status takes. */
static bool scan(void) {
    unsigned irqs = 0, empty = 0;
    for (unsigned page = 0; page < PAGES && empty < 2; ++page) {
        unsigned entries = 0;
        for (int at = PDT_TOP; at >= PDT_BOTTOM; at -= PDT_ENTRY) {
            uint8_t e[PDT_ENTRY];
            uint16_t base = (uint16_t)(page << 8);
            if (!rmi_read((uint16_t)(base + at), e, sizeof e)) return false;
            if (!e[5] || e[5] == 0xff) break;
            ++entries;
            struct function *f = e[5] == F01 ? &touch.f01 : e[5] == F12 ? &touch.f12 : 0;
            if (f && !f->found)
                *f = (struct function){base + e[0], base + e[2], base + e[3], irqs, true};
            irqs += e[4] & 7;
        }
        empty = entries ? 0 : empty + 1;
    }
    touch.irq_bytes = (irqs + 7) / 8;
    return touch.irq_bytes && touch.irq_bytes <= IRQ_BYTES;
}

/* Linux's rmi_read_register_desc: the presence register's size, the
 * presence register (the structure's size, then one bit per register),
 * then the structure: per register its size (a byte, else 16 or 32 bits)
 * and its subpackets, 7 bits a byte while bit 7 is set. */
static bool describe(uint16_t address, struct descriptor *d) {
    uint8_t presence_size, presence[35], structure[QUP_I2C_MAX_READ];
    if (!rmi_read(address, &presence_size, 1) || !presence_size ||
        presence_size > sizeof presence || !rmi_read(address + 1, presence, presence_size))
        return false;
    unsigned first = 1, size = presence[0];
    if (!size) {
        if (presence_size < 3) return false;
        first = 3;
        size = presence[1] | (unsigned)presence[2] << 8;
    }
    if (!size || size > sizeof structure || !rmi_read(address + 2, structure, size)) return false;
    d->count = 0;
    unsigned at = 0;
    for (unsigned byte = first; byte < presence_size; ++byte)
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (!(presence[byte] >> bit & 1)) continue;
            if (d->count == REGISTERS || at >= size) return false;
            uint32_t reg_size = structure[at++];
            if (!reg_size) {
                if (at + 2 > size) return false;
                reg_size = structure[at] | (uint32_t)structure[at + 1] << 8;
                at += 2;
            }
            if (!reg_size) {
                if (at + 4 > size) return false;
                reg_size = structure[at] | (uint32_t)structure[at + 1] << 8 |
                           (uint32_t)structure[at + 2] << 16 | (uint32_t)structure[at + 3] << 24;
                at += 4;
            }
            bool subpacket0 = false, more;
            unsigned subpacket_byte = 0;
            do {
                if (at >= size) return false;
                if (!subpacket_byte++) subpacket0 = structure[at] & 1;
                more = structure[at++] & 0x80;
            } while (more);
            if (reg_size > 0xffff) return false;
            d->reg[d->count] = (uint8_t)((byte - first) * 8 + bit);
            d->size[d->count] = (uint16_t)reg_size;
            d->subpacket0[d->count] = subpacket0;
            ++d->count;
        }
    return true;
}

static bool sensor(void) {
    uint8_t general;
    if (!rmi_read(touch.f12.query, &general, 1) || !(general & 1)) return false;
    static struct descriptor control, data;
    /* The query, control and data descriptors follow, 3 addresses each;
     * the queries' is not needed. */
    if (!describe(touch.f12.query + 4, &control) || !describe(touch.f12.query + 7, &data))
        return false;
    /* Data1: its byte offset in the data block read from F12's data base. */
    unsigned offset = 0;
    bool found = false;
    for (unsigned i = 0; i < data.count && !found; ++i) {
        if (data.reg[i] == 1) {
            found = true;
            touch.objects = data.size[i] / OBJECT_BYTES;
        } else offset += data.size[i];
    }
    touch.data1_offset = offset;
    if (!found || !touch.objects) return false;
    if (touch.objects > OBJECTS) touch.objects = OBJECTS;
    if (touch.data1_offset + touch.objects * OBJECT_BYTES > QUP_I2C_MAX_READ) return false;
    /* Control8, at its index among the control registers: max X and Y. */
    for (unsigned i = 0; i < control.count; ++i) {
        if (control.reg[i] != 8) continue;
        uint8_t tuning[4];
        if (!control.subpacket0[i] || control.size[i] < 4 ||
            !rmi_read(touch.f12.control + i, tuning, sizeof tuning))
            return false;
        touch.max_x = tuning[0] | (uint32_t)tuning[1] << 8;
        touch.max_y = tuning[2] | (uint32_t)tuning[3] << 8;
        return touch.max_x && touch.max_y;
    }
    return false;
}

/* F01's control: running (never dozing, as Linux's hammerhead device tree
 * asks) or the sensor asleep, marked configured so that a reset of the
 * controller shows as unconfigured; then only F01's and F12's position
 * interrupts enabled. */
static bool configure(bool asleep) {
    uint8_t control;
    if (!rmi_read(touch.f01.control, &control, 1)) return false;
    control &= (uint8_t)~(F01_SLEEP_MASK | F01_NOSLEEP);
    control |= F01_CONFIGURED | (asleep ? F01_SENSOR_SLEEP : F01_NOSLEEP);
    if (!rmi_write(touch.f01.control, control)) return false;
    uint8_t enable[IRQ_BYTES] = {0};
    enable[touch.f01.irq / 8] |= (uint8_t)(1 << touch.f01.irq % 8);
    enable[touch.f12.irq / 8] |= (uint8_t)(1 << touch.f12.irq % 8);
    for (unsigned i = 0; i < touch.irq_bytes; ++i)
        if (!rmi_write(touch.f01.control + 1 + i, enable[i])) return false;
    return true;
}

/* F01's device status and interrupt status (clearing them). */
static bool status(uint8_t *out) { return rmi_read(touch.f01.data, out, 1 + touch.irq_bytes); }

const char *nexus5_touch_init(uint32_t width, uint32_t height) {
    touch = (struct touch_state){.slot = NO_SLOT, .page = ~0u, .width = width, .height = height};
    if (!width || !height) return "touch screen: no screen size";
    /* Pins as Linux's device tree sets them: the attention line an input
     * (left an output driving low), and reset driven high (released, as
     * left); then the bus (i2c.c: the bootloader leaves its pins plain
     * GPIOs and its core clock off). */
    mmio_write32(nexus5_gpio(ATTENTION), 0);
    mmio_write32(nexus5_gpio(RESET) + 4, PIN_OUT);
    mmio_write32(nexus5_gpio(RESET), PIN_OUTPUT | PIN_PULL_UP);
    barrier();
    if (!nexus5_i2c_start(&bus, SDA, SCL)) return "touch screen: its I2C bus does not start";
    if (!answers()) {
        /* Held in reset until now, or stuck: a reset pulse, then its start. */
        mmio_write32(nexus5_gpio(RESET) + 4, 0);
        delay_us(RESET_US);
        mmio_write32(nexus5_gpio(RESET) + 4, PIN_OUT);
        bool up = false;
        for (uint64_t start = time_us(); !up && time_us() - start < START_US;) {
            uint64_t wait = time_us();
            while (time_us() - wait < ANSWER_POLL_US) nexus5_usb_poll();
            up = answers();
        }
        if (!up) return "touch screen: the controller does not answer";
    }
    if (!scan() || !touch.f01.found || !touch.f12.found)
        return "touch screen: no RMI4 device control and 2D sensor";
    uint8_t state[1 + IRQ_BYTES];
    if (!status(state)) return "touch screen: unreadable";
    if (state[0] & F01_BOOTLOADER) return "touch screen: controller firmware missing";
    if (!sensor()) return "touch screen: unknown 2D sensor layout";
    if (!configure(false) || !status(state)) return "touch screen: not configurable";
    touch.ready = true;
    return 0;
}

static bool contact(uint8_t type) {
    return type == OBJECT_FINGER || type == OBJECT_STYLUS || type == OBJECT_UNCLASSIFIED ||
           type == OBJECT_GLOVED;
}

static uint32_t scale(uint32_t value, uint32_t max, uint32_t size) {
    if (value > max) value = max;
    return (uint32_t)((uint64_t)value * (size - 1) / max);
}

/* The followed finger from F12's objects. */
static bool follow(const uint8_t *objects, struct pointer_event *event) {
    if (touch.slot == NO_SLOT) {
        for (unsigned i = 0; i < touch.objects; ++i)
            if (contact(objects[i * OBJECT_BYTES])) {
                touch.slot = (uint8_t)i;
                break;
            }
        if (touch.slot == NO_SLOT) return false;
    }
    const uint8_t *o = objects + touch.slot * OBJECT_BYTES;
    if (!contact(o[0])) {
        touch.slot = NO_SLOT;
        touch.down = false;
        *event = (struct pointer_event){touch.x, touch.y, false, false};
        return true;
    }
    uint32_t x = scale(o[1] | (uint32_t)o[2] << 8, touch.max_x, touch.width);
    uint32_t y = scale(o[3] | (uint32_t)o[4] << 8, touch.max_y, touch.height);
    if (touch.down && x == touch.x && y == touch.y) return false;
    touch.down = true;
    touch.x = x;
    touch.y = y;
    *event = (struct pointer_event){x, y, true, false};
    return true;
}

/* A failed read: after a few in a row, the gesture is cancelled. */
static bool failed(struct pointer_event *event) {
    touch.retry_after = time_us() + RETRY_US;
    if (++touch.failures < FAILURES || !touch.down) return false;
    touch.down = false;
    touch.slot = NO_SLOT;
    *event = (struct pointer_event){touch.x, touch.y, false, true};
    return true;
}

bool nexus5_touch_poll(struct pointer_event *event) {
    if (!touch.ready || touch.asleep || loop_time_us() < touch.retry_after) return false;
    uint8_t state[1 + IRQ_BYTES];
    if (touch.waking) {
        /* A sleeping sensor raises no attention: wake it again. */
        if (!configure(false)) {
            touch.retry_after = time_us() + RETRY_US;
            return false;
        }
        touch.waking = false;
        (void)status(state);
    }
    if (mmio_read32(nexus5_gpio(ATTENTION) + 4) & PIN_IN) return false;
    if (!status(state)) return failed(event);
    touch.failures = 0;
    if (state[0] & F01_UNCONFIGURED) {
        /* The controller restarted (its page back to 0): set it up again;
         * any gesture is over. */
        touch.page = ~0u;
        if (!configure(false)) return failed(event);
        if (!touch.down) return false;
        touch.down = false;
        touch.slot = NO_SLOT;
        *event = (struct pointer_event){touch.x, touch.y, false, true};
        return true;
    }
    if (!(state[1 + touch.f12.irq / 8] >> touch.f12.irq % 8 & 1)) return false;
    /* Objects up to the followed one; all of them to find one. */
    unsigned objects = touch.slot == NO_SLOT ? touch.objects : touch.slot + 1u;
    uint8_t data[QUP_I2C_MAX_READ];
    if (!rmi_read(touch.f12.data, data, touch.data1_offset + objects * OBJECT_BYTES))
        return failed(event);
    touch.failures = 0;
    return follow(data + touch.data1_offset, event);
}

void nexus5_touch_standby(bool on) {
    if (!touch.ready) return;
    touch.down = false;
    touch.slot = NO_SLOT;
    touch.asleep = on;
    touch.waking = false;
    uint8_t state[1 + IRQ_BYTES];
    /* Waking clears what was pending, so a touch starts afresh; a wake the
     * controller did not take is tried again from the polls. */
    if (configure(on)) {
        if (!on) (void)status(state);
    } else if (!on) {
        touch.waking = true;
        touch.retry_after = time_us() + RETRY_US;
    }
}
