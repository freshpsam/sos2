#include "input.h"
#include "platform.h"

static uint8_t read8(uintptr_t address) { return *(volatile uint8_t *)address; }

static void write8(uintptr_t address, uint8_t value) { *(volatile uint8_t *)address = value; }

/* Virtio 1.2, MMIO transport and input device, with a split receive queue.
 * DMA buffers use supervisor-only physical identity mappings.
 * DMA buffers must remain uncached or receive explicit cache maintenance if
 * the platform later enables a non-coherent cache configuration. */
enum {
    REG_MAGIC = 0x000,
    REG_VERSION = 0x004,
    REG_DEVICE_ID = 0x008,
    REG_DEVICE_FEATURES = 0x010,
    REG_DEVICE_FEATURES_SEL = 0x014,
    REG_DRIVER_FEATURES = 0x020,
    REG_DRIVER_FEATURES_SEL = 0x024,
    REG_QUEUE_SEL = 0x030,
    REG_QUEUE_MAX = 0x034,
    REG_QUEUE_NUM = 0x038,
    REG_QUEUE_READY = 0x044,
    REG_QUEUE_NOTIFY = 0x050,
    REG_INTERRUPT_STATUS = 0x060,
    REG_INTERRUPT_ACK = 0x064,
    REG_STATUS = 0x070,
    REG_QUEUE_DESC = 0x080,
    REG_QUEUE_DRIVER = 0x090,
    REG_QUEUE_DEVICE = 0x0a0,
    REG_CONFIG_GENERATION = 0x0fc,
    REG_CONFIG = 0x100,
    STATUS_ACK = 1,
    STATUS_DRIVER = 2,
    STATUS_DRIVER_OK = 4,
    STATUS_FEATURES_OK = 8,
    STATUS_NEEDS_RESET = 64,
    STATUS_FAILED = 128,
    QUEUE_SIZE = 64,
    DESCRIPTOR_WRITE = 2,
    AVAIL_NO_INTERRUPT = 1,
    INPUT_DEVICE_ID = 18,
    CONFIG_ABS_INFO = 0x12,
    EV_SYN = 0,
    EV_KEY = 1,
    EV_REL = 2,
    EV_ABS = 3,
    SYN_REPORT = 0,
    SYN_DROPPED = 3,
    ABS_X = 0,
    ABS_Y = 1,
    BTN_LEFT = 0x110,
};

struct descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
};

struct available_ring {
    uint16_t flags, index, ring[QUEUE_SIZE];
};

struct used_element {
    uint32_t id, length;
};

struct used_ring {
    uint16_t flags, index;
    struct used_element ring[QUEUE_SIZE];
};

struct input_event {
    uint16_t type, code;
    uint32_t value;
};

struct axis_range {
    int32_t minimum, maximum;
};

_Static_assert(sizeof(struct descriptor) == 16, "virtio descriptor layout");
_Static_assert(sizeof(struct input_event) == 8, "virtio input event layout");
_Static_assert(offsetof(struct used_ring, ring) == 4, "virtio used ring layout");

/* Independent queues prevent keyboard negotiation/reset from disturbing the tablet. */
struct transport {
    struct descriptor descriptors[QUEUE_SIZE] __attribute__((aligned(16)));
    volatile struct available_ring available __attribute__((aligned(16)));
    volatile struct used_ring used __attribute__((aligned(16)));
    volatile struct input_event buffers[QUEUE_SIZE] __attribute__((aligned(16)));
    uintptr_t base;
    uint16_t used_index;
    bool active;
};
static struct transport tablet, keyboard;
static struct axis_range axes[2];
static uint32_t screen_width, screen_height;
static struct pointer_event pointer;
static int32_t wheel_x, wheel_y;

static struct {
    struct {
        uint16_t code, value;
    } report[32];

    unsigned count, next;
    bool ready, pressed[256], caps;
    uint16_t repeat_key;
    uint64_t repeat_at;
} keys;

static uint32_t read_reg(struct transport *d, unsigned reg) {
    uint32_t value = mmio_read32(d->base + reg);
    barrier();
    return value;
}

static void write_reg(struct transport *d, unsigned reg, uint32_t value) {
    barrier();
    mmio_write32(d->base + reg, value);
    barrier();
}

static void write_address(struct transport *d, unsigned reg, const volatile void *address) {
    uint64_t physical = (uintptr_t)address;
    write_reg(d, reg, (uint32_t)physical);
    write_reg(d, reg + 4, (uint32_t)(physical >> 32));
}

static bool fail(struct transport *d) {
    d->active = false;
    if (d->base) write_reg(d, REG_STATUS, read_reg(d, REG_STATUS) | STATUS_FAILED);
    return false;
}

static const char *transport_failure(struct transport *d) {
    uint32_t status = read_reg(d, REG_STATUS);
    if ((status & (STATUS_FAILED | STATUS_NEEDS_RESET)) || !(status & STATUS_DRIVER_OK))
        return "device stopped or requests reset";
    uint32_t interrupts = read_reg(d, REG_INTERRUPT_STATUS) & 3;
    if (interrupts & 2) return "device configuration changed; reinitialization required";
    if (interrupts & 1) write_reg(d, REG_INTERRUPT_ACK, 1);
    return 0;
}

static bool read_axis(struct transport *d, unsigned axis, struct axis_range *range) {
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        uint32_t generation = read_reg(d, REG_CONFIG_GENERATION);
        write8(d->base + REG_CONFIG, CONFIG_ABS_INFO);
        write8(d->base + REG_CONFIG + 1, (uint8_t)axis);
        barrier();
        uint8_t size = read8(d->base + REG_CONFIG + 2);
        int32_t minimum = (int32_t)read_reg(d, REG_CONFIG + 8),
                maximum = (int32_t)read_reg(d, REG_CONFIG + 12);
        if (generation != read_reg(d, REG_CONFIG_GENERATION)) continue;
        if (size < 20 || minimum >= maximum) return false;
        *range = (struct axis_range){minimum, maximum};
        return true;
    }
    return false;
}

static bool is_keyboard(struct transport *d) {
    /* Virtio EV_BITS bitmap: require Enter and A, not just pointer buttons. */
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        uint32_t generation = read_reg(d, REG_CONFIG_GENERATION);
        write8(d->base + REG_CONFIG, 0x11);
        write8(d->base + REG_CONFIG + 1, EV_KEY);
        barrier();
        unsigned size = read8(d->base + REG_CONFIG + 2);
        unsigned bits = size >= 4 && size <= 128 ? read8(d->base + REG_CONFIG + 8 + 3) : 0;
        if (generation == read_reg(d, REG_CONFIG_GENERATION)) return (bits & 0x50) == 0x50;
    }
    return false;
}

static const char *negotiate(struct transport *d) {
    write_reg(d, REG_STATUS, 0);
    uint64_t start = time_us();
    unsigned polls = 0;
    while (read_reg(d, REG_STATUS))
        if (time_us() - start > 100000 || ++polls == 100000) return "device reset timed out";
    write_reg(d, REG_STATUS, STATUS_ACK);
    write_reg(d, REG_STATUS, STATUS_ACK | STATUS_DRIVER);
    write_reg(d, REG_DEVICE_FEATURES_SEL, 1);
    if (!(read_reg(d, REG_DEVICE_FEATURES) & 1)) return "device lacks VIRTIO_F_VERSION_1";
    write_reg(d, REG_DRIVER_FEATURES_SEL, 0);
    write_reg(d, REG_DRIVER_FEATURES, 0);
    write_reg(d, REG_DRIVER_FEATURES_SEL, 1);
    write_reg(d, REG_DRIVER_FEATURES, 1);
    write_reg(d, REG_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK);
    return read_reg(d, REG_STATUS) & STATUS_FEATURES_OK ? 0 : "feature negotiation rejected";
}

static bool enable_queue(struct transport *d) {
    write_reg(d, REG_QUEUE_SEL, 0);
    if (read_reg(d, REG_QUEUE_READY)) return fail(d);
    if (read_reg(d, REG_QUEUE_MAX) < QUEUE_SIZE) return fail(d);
    memset(d->descriptors, 0, sizeof(d->descriptors));
    memset((void *)&d->available, 0, sizeof(d->available));
    memset((void *)&d->used, 0, sizeof(d->used));
    memset((void *)d->buffers, 0, sizeof(d->buffers));
    d->used_index = 0;
    for (unsigned i = 0; i < QUEUE_SIZE; ++i) {
        d->descriptors[i] = (struct descriptor){(uintptr_t)&d->buffers[i], sizeof(d->buffers[i]),
                                                DESCRIPTOR_WRITE, 0};
        d->available.ring[i] = (uint16_t)i;
    }
    d->available.flags = AVAIL_NO_INTERRUPT;
    barrier();
    d->available.index = QUEUE_SIZE;
    barrier();
    write_reg(d, REG_QUEUE_NUM, QUEUE_SIZE);
    write_address(d, REG_QUEUE_DESC, d->descriptors);
    write_address(d, REG_QUEUE_DRIVER, &d->available);
    write_address(d, REG_QUEUE_DEVICE, &d->used);
    write_reg(d, REG_QUEUE_READY, 1);
    write_reg(d, REG_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    uint32_t status = read_reg(d, REG_STATUS);
    if (!(status & STATUS_DRIVER_OK) || (status & (STATUS_FAILED | STATUS_NEEDS_RESET)))
        return fail(d);
    d->active = true;
    write_reg(d, REG_QUEUE_NOTIFY, 0);
    return true;
}

static bool find_device(struct transport *d, const uintptr_t *bases, size_t count) {
    const char *rejected = 0;
    struct transport *other = d == &tablet ? &keyboard : &tablet;
    for (size_t i = 0; i < count; ++i) {
        uintptr_t base = bases[i];
        if (!base || (other->active && base == other->base) ||
            mmio_read32(base + REG_MAGIC) != 0x74726976)
            continue;
        if (mmio_read32(base + REG_VERSION) != 2) continue;
        if (mmio_read32(base + REG_DEVICE_ID) != INPUT_DEVICE_ID) continue;
        d->base = base;
        if (d == &keyboard && !is_keyboard(d)) {
            d->base = 0;
            continue;
        }
        rejected = negotiate(d);
        if (!rejected && d == &tablet &&
            (!read_axis(d, ABS_X, &axes[0]) || !read_axis(d, ABS_Y, &axes[1])))
            rejected = "device does not expose stable absolute X/Y ranges";
        if (!rejected) return enable_queue(d);
        write_reg(d, REG_STATUS, 0);
        d->base = 0;
    }
    return fail(d);
}

/* Returns 1 for a copied event, 0 for empty, -1 after a bounded validation error. */
static int receive(struct transport *d, struct input_event *event) {
    uint16_t producer = d->used.index;
    barrier();
    uint16_t pending = (uint16_t)(producer - d->used_index);
    if (!pending) return 0;
    if (pending > QUEUE_SIZE) {
        fail(d);
        return -1;
    }
    struct used_element entry = {d->used.ring[d->used_index % QUEUE_SIZE].id,
                                 d->used.ring[d->used_index % QUEUE_SIZE].length};
    if (entry.id >= QUEUE_SIZE || entry.length != sizeof(*event)) {
        fail(d);
        return -1;
    }
    barrier();
    *event = (struct input_event){d->buffers[entry.id].type, d->buffers[entry.id].code,
                                  d->buffers[entry.id].value};
    ++d->used_index;
    uint16_t available = d->available.index;
    d->available.ring[available % QUEUE_SIZE] = (uint16_t)entry.id;
    barrier();
    d->available.index = (uint16_t)(available + 1);
    barrier();
    write_reg(d, REG_QUEUE_NOTIFY, 0);
    return 1;
}

static uint32_t normalize(int32_t value, const struct axis_range *range, uint32_t extent) {
    if (value <= range->minimum) return 0;
    if (value >= range->maximum) return extent - 1;
    uint64_t offset = (uint64_t)((int64_t)value - range->minimum),
             span = (uint64_t)((int64_t)range->maximum - range->minimum);
    return (uint32_t)(offset * (extent - 1) / span);
}

bool input_init(const uintptr_t *bases, size_t count, uint32_t width, uint32_t height) {
    tablet.active = false;
    tablet.base = 0;
    pointer = (struct pointer_event){0};
    wheel_x = wheel_y = 0;
    screen_width = width;
    screen_height = height;
    if (!width || !height || (!bases && count)) return fail(&tablet);
    if (!find_device(&tablet, bases, count)) return false;
    return true;
}

static bool pointer_cancel(struct pointer_event *event, const char *reason) {
    if (reason) fail(&tablet);
    pointer.down = false;
    pointer.cancelled = true;
    wheel_x = wheel_y = 0;
    *event = pointer;
    return true;
}

static int32_t add_wheel(int32_t total, int32_t value) {
    if (value > 127) value = 127;
    if (value < -127) value = -127;
    total += value;
    return total > 127 ? 127 : total < -127 ? -127 : total;
}

bool input_poll_extended(struct pointer_event *event, int32_t *dx, int32_t *dy) {
    if (dx) *dx = 0;
    if (dy) *dy = 0;
    if (!tablet.active || !event) return false;
    const char *reason = transport_failure(&tablet);
    if (reason) return pointer_cancel(event, reason);
    for (unsigned budget = 0; budget < QUEUE_SIZE; ++budget) {
        struct input_event received;
        int status = receive(&tablet, &received);
        if (status < 0) return pointer_cancel(event, 0);
        if (!status) break;
        if (received.type == EV_ABS && received.code == ABS_X)
            pointer.x = normalize((int32_t)received.value, &axes[0], screen_width);
        else if (received.type == EV_ABS && received.code == ABS_Y)
            pointer.y = normalize((int32_t)received.value, &axes[1], screen_height);
        else if (received.type == EV_KEY && received.code == BTN_LEFT && received.value <= 1)
            pointer.down = received.value != 0;
        else if (received.type == EV_REL && received.code == 8)
            wheel_y = add_wheel(wheel_y, (int32_t)received.value);
        else if (received.type == EV_REL && received.code == 6)
            wheel_x = add_wheel(wheel_x, (int32_t)received.value);
        else if (received.type == EV_SYN && received.code == SYN_DROPPED)
            return pointer_cancel(event, "input stream lost synchronization");
        else if (received.type == EV_SYN && received.code == SYN_REPORT) {
            reason = transport_failure(&tablet);
            if (reason) return pointer_cancel(event, reason);
            *event = pointer;
            if (dx) *dx = wheel_x;
            if (dy) *dy = wheel_y;
            wheel_x = wheel_y = 0;
            return true;
        }
    }
    return false;
}

bool keyboard_init(const uintptr_t *bases, size_t count) {
    keyboard.active = false;
    keyboard.base = 0;
    memset(&keys, 0, sizeof(keys));
    if (!bases && count) return fail(&keyboard);
    if (!find_device(&keyboard, bases, count)) return false;
    return true;
}

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

static bool keyboard_cancel(struct user_input_event *event, const char *reason) {
    if (reason) fail(&keyboard);
    memset(&keys, 0, sizeof(keys));
    *event = (struct user_input_event){.type = INPUT_RESET};
    return true;
}

bool keyboard_poll(struct user_input_event *event) {
    if (!keyboard.active || !event) return false;
    const char *reason = transport_failure(&keyboard);
    if (reason) return keyboard_cancel(event, reason);
    for (unsigned budget = 0; budget < QUEUE_SIZE; ++budget) {
        if (keys.ready) {
            while (keys.next < keys.count) {
                unsigned at = keys.next++;
                if (key_transition(event, keys.report[at].code, keys.report[at].value)) return true;
            }
            keys.ready = false;
            keys.next = keys.count = 0;
        }
        struct input_event received;
        int status = receive(&keyboard, &received);
        if (status < 0) return keyboard_cancel(event, 0);
        if (!status) break;
        if (received.type == EV_KEY && received.code > 0 && received.code < 256 &&
            received.value <= 2) {
            if (keys.count == 32) return keyboard_cancel(event, "keyboard report too large");
            keys.report[keys.count].code = received.code;
            keys.report[keys.count++].value = received.value;
        } else if (received.type == EV_SYN && received.code == SYN_DROPPED)
            return keyboard_cancel(event, "input stream lost synchronization");
        else if (received.type == EV_SYN && received.code == SYN_REPORT) {
            reason = transport_failure(&keyboard);
            if (reason) return keyboard_cancel(event, reason);
            keys.ready = true;
        }
    }
    if (!keys.count && keys.repeat_key && time_us() >= keys.repeat_at)
        return key_transition(event, keys.repeat_key, 2);
    return false;
}

/* QEMU virt's power button: its gpio-key holds a PL061 input high for about
 * 100 ms per press (QMP system_powerdown, or Machine > Power Down in a QEMU
 * window). The pin is only read, never driven or configured.
 * https://developer.arm.com/documentation/ddi0190/b (PL061 TRM, 3.3.1 GPIODATA)
 * https://gitlab.com/qemu-project/qemu/-/blob/master/hw/gpio/gpio_key.c */
enum { POWER_SAMPLE_US = 10000, POWER_DEBOUNCE_US = 20000 };

static uintptr_t power_data;
static bool power_candidate, power_stable;
static uint64_t power_sampled, power_since;

bool power_key_init(uintptr_t gpio, unsigned pin) {
    power_data = 0;
    if (!gpio || pin > 7) return false;
    /* PrimeCell peripheral IDs 0..1 (PL061) and component IDs. */
    static const uint8_t id[6] = {0x61, 0x10, 0x0d, 0xf0, 0x05, 0xb1};
    static const uint16_t offset[6] = {0xfe0, 0xfe4, 0xff0, 0xff4, 0xff8, 0xffc};
    for (unsigned i = 0; i < 6; ++i)
        if ((mmio_read32(gpio + offset[i]) & 0xff) != id[i]) return false;
    /* GPIODATA reads only the pins selected by address bits [9:2]. A key held
     * during boot must be released before a press counts. */
    power_data = gpio + (4u << pin);
    power_candidate = power_stable = mmio_read32(power_data) != 0;
    power_sampled = power_since = time_us();
    return true;
}

bool power_key_pressed(void) {
    if (!power_data) return false;
    uint64_t now = time_us();
    if (now - power_sampled < POWER_SAMPLE_US) return false;
    power_sampled = now;
    bool pressed = mmio_read32(power_data) != 0;
    if (pressed != power_candidate) {
        power_candidate = pressed;
        power_since = now;
        return false;
    }
    if (now - power_since < POWER_DEBOUNCE_US || pressed == power_stable) return false;
    power_stable = pressed;
    return pressed;
}
