#include "pc.h"
#include "platform.h"
#include "kernel_io.h"
#include "machine.h"

/* USB on a PC's xHCI controllers, polled: each controller taken from the
 * firmware and reset, with one command ring and one event ring, and devices
 * on its root ports (hubs come later) enumerated at start and as they come
 * and go. Each device's first configuration is set; its HID interfaces (an
 * interrupt IN endpoint) go to usb_hid.c and a mass storage one (bulk IN and
 * OUT, BOT) to usb_storage.c, as many as its rings take. Devices are numbered
 * across the controllers, and the class drivers name them by that number.
 * Every structure a controller reads or writes is static, below 4 GiB,
 * aligned so that contexts keep within a page and rings within 64 KiB, and
 * x86's DMA is coherent. Waits poll the event rings, yielding on the service
 * worker; an endpoint that halts or times out is reset or stopped and its
 * ring started again past what was queued.
 * https://www.intel.com/content/www/us/en/io/universal-serial-bus/extensible-host-controler-interface-usb-xhci.html
 * (xHCI 1.2: 4.2 and 4.3, initialization and enumeration; 4.6, the commands;
 * 4.10.2, errors; 6, the data structures; 5, the registers)
 * https://www.usb.org/document-library/usb-20-specification (USB 2.0: 7.1.7.3
 * and 7.1.7.5, the attach debounce and reset recovery; 9.2.6.3, SET_ADDRESS's) */
enum {
    CAPLENGTH = 0x00,
    HCSPARAMS1 = 0x04,
    HCSPARAMS2 = 0x08,
    HCCPARAMS1 = 0x10,
    DBOFF = 0x14,
    RTSOFF = 0x18,
    USBCMD = 0x00,
    USBSTS = 0x04,
    PAGESIZE = 0x08,
    CRCR = 0x18,
    DCBAAP = 0x30,
    CONFIG = 0x38,
    PORTSC = 0x400,
    IMAN = 0x20,
    ERSTSZ = 0x28,
    ERSTBA = 0x30,
    ERDP = 0x38,
    RUN = 1 << 0,
    HCRST = 1 << 1,
    HALTED = 1 << 0,
    NOT_READY = 1 << 11,
    /* PORTSC: connected, enabled, reset, power; the changes, which a write
     * of one clears; and what a write keeps. */
    CCS = 1 << 0,
    PED = 1 << 1,
    PR = 1 << 4,
    PP = 1 << 9,
    CSC = 1 << 17,
    PRC = 1 << 21,
    PORT_CHANGES = 0x7f << 17,
    PORT_KEEP = PP | 0xf << 5 | 3 << 14 | 7 << 25,
};

enum {
    TRB_NORMAL = 1,
    TRB_SETUP = 2,
    TRB_DATA = 3,
    TRB_STATUS = 4,
    TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9,
    TRB_DISABLE_SLOT = 10,
    TRB_ADDRESS_DEVICE = 11,
    TRB_CONFIGURE_ENDPOINT = 12,
    TRB_EVALUATE_CONTEXT = 13,
    TRB_RESET_ENDPOINT = 14,
    TRB_STOP_ENDPOINT = 15,
    TRB_SET_DEQUEUE = 16,
    TRB_TRANSFER_EVENT = 32,
    TRB_COMMAND_COMPLETION = 33,
    TRB_PORT_STATUS = 34,
    COMPLETION_SUCCESS = 1,
    COMPLETION_SHORT = 13,
    COMPLETION_STOPPED = 26,
    COMPLETION_STOPPED_LENGTH = 27,
    COMPLETION_STOPPED_SHORT = 28,
    /* An endpoint context's state (6.2.3). */
    ENDPOINT_RUNNING = 1,
    ENDPOINT_HALTED = 2,
    ENDPOINT_STOPPED = 3,
};

#define TRB_CYCLE      1u
#define TRB_TOGGLE     2u
#define TRB_ISP        (1u << 2)
#define TRB_IOC        (1u << 5)
#define TRB_IDT        (1u << 6)
#define TRB_IN         (1u << 16)
#define TRB_TYPE(type) ((uint32_t)(type) << 10)
#define RING           64
#define CONTROLLERS    4
#define SLOTS          16 /* a controller's */
#define DEVICES        16 /* the controllers' together */
#define RINGS          4  /* endpoint 0 and three more a device */
#define PORTS          32
#define SCRATCHPADS    32
#define CONFIGURATION  512
/* USB 2.0's reset recovery (10 ms) and SET_ADDRESS's (2 ms), with Linux's
 * margins, and a connection's debounce. */
#define RESET_RECOVERY_US   50000
#define ADDRESS_RECOVERY_US 10000
#define DEBOUNCE_US         100000

struct trb {
    uint64_t parameter;
    uint32_t status, control;
};

struct ring {
    struct trb *trbs;
    unsigned enqueue;
    uint32_t cycle;
};

/* A transfer's completion, for the waiter or the class driver. */
struct transfer {
    struct ring ring;
    uint8_t dci, code;
    bool done;
    uint32_t residual;
    /* An interrupt IN endpoint's buffer, started again after each report. */
    uint8_t *report;
    uint32_t report_length;
};

struct controller;

struct device {
    uint8_t input[33 * 64] __attribute__((aligned(4096)));
    uint8_t output[32 * 64] __attribute__((aligned(2048)));
    struct trb rings[RINGS][RING] __attribute__((aligned(1024)));
    struct transfer transfers[RINGS];
    struct controller *host;
    uint32_t generation;       /* a new one for each device the number serves */
    uint8_t slot, port, speed; /* its slot and root port on its controller */
    bool used;
};

struct controller {
    struct trb command_trbs[RING] __attribute__((aligned(1024)));
    struct trb events[RING] __attribute__((aligned(1024)));

    struct {
        uint64_t base;
        uint32_t size, reserved;
    } segment_table[1] __attribute__((aligned(64)));

    uint64_t dcbaa[SLOTS + 1] __attribute__((aligned(256)));
    uint64_t scratchpad_array[SCRATCHPADS] __attribute__((aligned(256)));
    const struct pci_function *pci;
    uintptr_t capabilities, operational, runtime, doorbells;
    unsigned slot_count, port_count, context_size;
    uint8_t port_protocol[PORTS + 1];
    uint8_t device[SLOTS + 1]; /* the device in each of its slots */
    bool running, port_changed[PORTS + 1];
    struct ring commands;
    unsigned event_dequeue;
    uint32_t event_cycle;
    uint64_t command_trb;
    bool command_done;
    uint8_t command_code, command_slot;
};

static struct controller controllers[CONTROLLERS];
static uint8_t scratchpads[CONTROLLERS][SCRATCHPADS][4096] __attribute__((aligned(4096)));
static struct device devices[DEVICES + 1];
static uint8_t descriptor[CONFIGURATION] __attribute__((aligned(CONFIGURATION)));
static unsigned controller_count;
static uint32_t generations;
/* enumerating: the ports are being walked; command_waiting: a command is
 * waited for, on the service worker or the loop, which the loop's port
 * changes then leave alone. */
static bool started, running, enumerating, command_waiting;

static uint32_t op_read(const struct controller *c, unsigned offset) {
    return mmio_read32(c->operational + offset);
}

static void op_write(const struct controller *c, unsigned offset, uint32_t value) {
    mmio_write32(c->operational + offset, value);
}

static void op_write64(const struct controller *c, unsigned offset, uint64_t value) {
    mmio_write32(c->operational + offset, (uint32_t)value);
    mmio_write32(c->operational + offset + 4, (uint32_t)(value >> 32));
}

static void rt_write64(const struct controller *c, unsigned offset, uint64_t value) {
    mmio_write32(c->runtime + offset, (uint32_t)value);
    mmio_write32(c->runtime + offset + 4, (uint32_t)(value >> 32));
}

static uint32_t port_read(const struct controller *c, unsigned port) {
    return op_read(c, PORTSC + 0x10 * (port - 1));
}

static void port_write(const struct controller *c, unsigned port, uint32_t value) {
    op_write(c, PORTSC + 0x10 * (port - 1), value);
}

/* A port's status, its changes cleared: the controller reports a port's
 * next change only once none is left set. */
static uint32_t port_acknowledge(const struct controller *c, unsigned port) {
    uint32_t status = port_read(c, port);
    port_write(c, port, (status & PORT_KEEP) | (status & PORT_CHANGES));
    return status;
}

/* Waits on a condition the caller polls, up to timeout_us. */
static bool expired(uint64_t deadline) {
    kernel_io_wait();
    return time_us() > deadline;
}

static void ring_init(struct ring *r, struct trb *trbs) {
    memset(trbs, 0, RING * sizeof(*trbs));
    r->trbs = trbs;
    r->enqueue = 0;
    r->cycle = 1;
    trbs[RING - 1].parameter = (uintptr_t)trbs;
    trbs[RING - 1].control = TRB_TYPE(TRB_LINK) | TRB_TOGGLE;
}

/* A TRB on the ring, its cycle bit written last; past the end the link TRB
 * hands over with the current cycle, which then turns. */
static struct trb *ring_push(struct ring *r, uint64_t parameter, uint32_t status,
                             uint32_t control) {
    struct trb *t = &r->trbs[r->enqueue];
    t->parameter = parameter;
    t->status = status;
    barrier();
    t->control = (control & ~TRB_CYCLE) | r->cycle;
    if (++r->enqueue == RING - 1) {
        struct trb *link = &r->trbs[RING - 1];
        barrier();
        link->control = (link->control & ~TRB_CYCLE) | r->cycle;
        r->cycle ^= 1;
        r->enqueue = 0;
    }
    return t;
}

static void ring_doorbell(const struct controller *c, unsigned slot, unsigned target) {
    barrier();
    mmio_write32(c->doorbells + 4 * slot, target);
}

static void *context(const struct controller *c, uint8_t *base, unsigned index) {
    return base + (size_t)index * c->context_size;
}

static struct transfer *transfer_for(unsigned id, unsigned dci) {
    if (!id || id > DEVICES || !devices[id].used) return 0;
    for (unsigned i = 0; i < RINGS; ++i)
        if (devices[id].transfers[i].dci == dci) return &devices[id].transfers[i];
    return 0;
}

static void transfer_event(const struct controller *c, const struct trb *e) {
    unsigned slot = e->control >> 24;
    unsigned id = slot && slot <= SLOTS ? c->device[slot] : 0;
    struct transfer *t = transfer_for(id, (e->control >> 16) & 31);
    if (!t) return;
    uint8_t code = (uint8_t)(e->status >> 24);
    /* A transfer stopped on the way to its ring's restart completes for no one. */
    if (code == COMPLETION_STOPPED || code == COMPLETION_STOPPED_LENGTH ||
        code == COMPLETION_STOPPED_SHORT)
        return;
    t->code = code;
    t->residual = e->status & 0xffffff;
    if (!t->report) {
        t->done = true;
        return;
    }
    if (t->code != COMPLETION_SUCCESS && t->code != COMPLETION_SHORT) {
        t->report = 0; /* halted: the device is going, or its driver gave up */
        return;
    }
    uint32_t length =
        t->report_length - (t->residual < t->report_length ? t->residual : t->report_length);
    usb_hid_report(id, t->dci, t->report, length);
    if (t->report) {
        ring_push(&t->ring, (uintptr_t)t->report, t->report_length,
                  TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
        ring_doorbell(c, slot, t->dci);
    }
}

static void poll(struct controller *c) {
    bool consumed = false;
    for (unsigned budget = 0; budget < RING; ++budget) {
        struct trb *slot = &c->events[c->event_dequeue];
        if ((slot->control & TRB_CYCLE) != c->event_cycle) break;
        barrier();
        struct trb e = *slot;
        if (++c->event_dequeue == RING) {
            c->event_dequeue = 0;
            c->event_cycle ^= 1;
        }
        consumed = true;
        unsigned type = (e.control >> 10) & 63;
        if (type == TRB_TRANSFER_EVENT) {
            transfer_event(c, &e);
        } else if (type == TRB_COMMAND_COMPLETION && e.parameter == c->command_trb) {
            c->command_code = (uint8_t)(e.status >> 24);
            c->command_slot = (uint8_t)(e.control >> 24);
            c->command_done = true;
        } else if (type == TRB_PORT_STATUS) {
            unsigned port = (unsigned)(e.parameter >> 24) & 0xff;
            if (port && port <= c->port_count) c->port_changed[port] = true;
        }
    }
    /* The dequeue pointer, with the handler busy flag cleared. */
    if (consumed) rt_write64(c, ERDP, (uintptr_t)&c->events[c->event_dequeue] | 8);
}

void xhci_poll(void) {
    for (unsigned i = 0; i < controller_count; ++i)
        if (controllers[i].running) poll(&controllers[i]);
}

static bool command(struct controller *c, uint64_t parameter, uint32_t control, uint8_t *slot) {
    c->command_done = false;
    c->command_trb = (uintptr_t)ring_push(&c->commands, parameter, 0, control);
    ring_doorbell(c, 0, 0);
    command_waiting = true;
    for (uint64_t deadline = time_us() + 1000000; !c->command_done;) {
        poll(c);
        if (!c->command_done && expired(deadline)) break;
    }
    command_waiting = false;
    if (!c->command_done) return false;
    if (slot) *slot = c->command_slot;
    return c->command_code == COMPLETION_SUCCESS;
}

/* A transfer's completion, while the device its number named is there. */
static bool wait_transfer(unsigned id, struct transfer *t, uint32_t timeout_us) {
    uint32_t generation = devices[id].generation;
    for (uint64_t deadline = time_us() + timeout_us; !t->done;) {
        xhci_poll();
        if (!devices[id].used || devices[id].generation != generation) return false;
        if (!t->done && expired(deadline)) return false;
    }
    return t->code == COMPLETION_SUCCESS || t->code == COMPLETION_SHORT;
}

static unsigned endpoint_state(const struct device *d, unsigned dci) {
    return ((volatile uint32_t *)context(d->host, (uint8_t *)d->output, dci))[0] & 7;
}

/* An endpoint that halted or timed out, ready again: reset if halted, or
 * stopped if still running with a transfer queued, then its ring started
 * again at the next TRB, past whatever was queued (4.6.8 to 4.6.10). With
 * toggle it is instead dropped and added again, which also starts its data
 * toggle afresh, as the device's does at CLEAR_FEATURE(ENDPOINT_HALT). */
static bool endpoint_restart(unsigned id, struct transfer *t, bool toggle) {
    struct device *d = &devices[id];
    if (!d->used) return false;
    struct controller *c = d->host;
    uint32_t target = (uint32_t)t->dci << 16 | (uint32_t)d->slot << 24;
    for (unsigned attempt = 0; attempt < 3 && endpoint_state(d, t->dci) != ENDPOINT_STOPPED;
         ++attempt) {
        unsigned state = endpoint_state(d, t->dci);
        if (state == ENDPOINT_HALTED) (void)command(c, 0, TRB_TYPE(TRB_RESET_ENDPOINT) | target, 0);
        else if (state == ENDPOINT_RUNNING)
            (void)command(c, 0, TRB_TYPE(TRB_STOP_ENDPOINT) | target, 0);
        else return false;
        if (!d->used) return false;
    }
    if (endpoint_state(d, t->dci) != ENDPOINT_STOPPED) return false;
    t->done = false;
    uint64_t next = (uintptr_t)&t->ring.trbs[t->ring.enqueue] | t->ring.cycle;
    if (!toggle) return command(c, next, TRB_TYPE(TRB_SET_DEQUEUE) | target, 0);
    memset(d->input, 0, sizeof(d->input));
    uint32_t *control = context(c, d->input, 0), *endpoint = context(c, d->input, 1 + t->dci);
    control[0] = 1u << t->dci;
    control[1] = 1u << t->dci | 1;
    memcpy(context(c, d->input, 1), context(c, d->output, 0), c->context_size);
    memcpy(endpoint, context(c, d->output, t->dci), c->context_size);
    endpoint[0] &= ~7u;
    endpoint[2] = (uint32_t)next;
    endpoint[3] = (uint32_t)(next >> 32);
    return command(c, (uintptr_t)d->input,
                   TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | (uint32_t)d->slot << 24, 0);
}

bool usb_control(unsigned id, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                 void *data, uint16_t length, uint32_t timeout_us) {
    struct transfer *t = transfer_for(id, 1);
    if (!t) return false;
    const struct device *d = &devices[id];
    bool in = type & 0x80;
    uint64_t setup = type | (uint64_t)request << 8 | (uint64_t)value << 16 | (uint64_t)index << 32 |
                     (uint64_t)length << 48;
    t->done = false;
    ring_push(&t->ring, setup, 8,
              TRB_TYPE(TRB_SETUP) | TRB_IDT | (length ? (in ? 3u : 2u) << 16 : 0));
    if (length)
        ring_push(&t->ring, (uintptr_t)data, length, TRB_TYPE(TRB_DATA) | (in ? TRB_IN : 0));
    ring_push(&t->ring, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (length && in ? 0 : TRB_IN));
    ring_doorbell(d->host, d->slot, 1);
    if (wait_transfer(id, t, timeout_us)) return true;
    /* A request refused (STALL) or failed halts endpoint 0, and one timed
     * out leaves it queued: either way it starts again past the request. */
    (void)endpoint_restart(id, t, false);
    return false;
}

bool usb_bulk(const struct usb_endpoint *e, void *data, uint32_t length, uint32_t *done,
              uint32_t timeout_us) {
    struct transfer *t = transfer_for(e->slot, e->dci);
    if (!t || length > 0x10000) return false;
    const struct device *d = &devices[e->slot];
    t->done = false;
    ring_push(&t->ring, (uintptr_t)data, length, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    ring_doorbell(d->host, d->slot, e->dci);
    bool ok = wait_transfer(e->slot, t, timeout_us);
    if (done) *done = t->done ? length - (t->residual < length ? t->residual : length) : 0;
    /* One that halted, or is still queued, is left behind. */
    if (!ok) (void)endpoint_restart(e->slot, t, false);
    return ok;
}

bool usb_interrupt_in(const struct usb_endpoint *e, void *data, uint32_t length) {
    struct transfer *t = transfer_for(e->slot, e->dci);
    if (!t || !length) return false;
    t->report = data;
    t->report_length = length;
    ring_push(&t->ring, (uintptr_t)data, length, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    ring_doorbell(devices[e->slot].host, devices[e->slot].slot, e->dci);
    return true;
}

bool usb_clear_halt(const struct usb_endpoint *e) {
    struct transfer *t = transfer_for(e->slot, e->dci);
    if (!t) return false;
    uint8_t address = (uint8_t)((e->dci / 2) | (e->dci & 1 ? 0x80 : 0));
    bool cleared = usb_control(e->slot, 0x02, 1, 0, address, 0, 0, 500000);
    return endpoint_restart(e->slot, t, true) && cleared;
}

static void release(unsigned id) {
    struct device *d = &devices[id];
    if (!id || id > DEVICES || !d->used) return;
    usb_detach(id);
    d->used = false;
    struct controller *c = d->host;
    (void)command(c, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)d->slot << 24, 0);
    c->dcbaa[d->slot] = 0;
    c->device[d->slot] = 0;
}

/* An endpoint's DCI: twice its number, plus one going in. */
static unsigned endpoint_dci(uint8_t address) { return (address & 15) * 2 + (address >> 7); }

/* An interrupt endpoint's interval as xHCI counts it, 2^n x 125 us. */
static unsigned interval(uint8_t speed, uint8_t value) {
    if (speed >= 3) return value >= 1 && value <= 16 ? value - 1u : 3u;
    unsigned n = 3;
    while (n < 10 && (1u << (n + 1)) <= value * 8u) ++n;
    return n;
}

static void endpoint_context(struct device *d, unsigned dci, unsigned type, uint16_t max_packet,
                             unsigned every, struct transfer *t) {
    uint32_t *ep = context(d->host, d->input, 1 + dci);
    memset(ep, 0, d->host->context_size);
    ep[0] = every << 16;
    ep[1] = 3u << 1 | type << 3 | (uint32_t)max_packet << 16;
    uint64_t dequeue = (uintptr_t)t->ring.trbs | 1;
    ep[2] = (uint32_t)dequeue;
    ep[3] = (uint32_t)(dequeue >> 32);
    ep[4] = (type == 4 ? 8u : max_packet) | (type == 7 ? (uint32_t)max_packet << 16 : 0);
}

struct interface {
    uint8_t number, class, subclass, protocol;
    uint16_t report_length;
    struct usb_endpoint in, out;
    uint8_t in_interval;
};

/* The first configuration's interfaces that class drivers take: HID with
 * an interrupt IN endpoint, mass storage (SCSI, bulk only) with bulk IN and
 * OUT. */
static unsigned parse_configuration(const uint8_t *c, unsigned size, unsigned id,
                                    struct interface *out, unsigned limit) {
    unsigned count = 0;
    struct interface *current = 0;
    for (unsigned at = 0; at + 2 <= size && c[at] >= 2 && at + c[at] <= size; at += c[at]) {
        const uint8_t *d = c + at;
        if (d[1] == 4 && d[0] >= 9) {
            current = 0;
            bool hid = d[5] == USB_CLASS_HID,
                 storage = d[5] == USB_CLASS_STORAGE && d[6] == 6 && d[7] == 0x50;
            if (!d[3] && (hid || storage) && count < limit) {
                current = &out[count++];
                *current = (struct interface){
                    .number = d[2], .class = d[5], .subclass = d[6], .protocol = d[7]};
            }
        } else if (d[1] == 0x21 && d[0] >= 9 && current && current->class == USB_CLASS_HID) {
            if (d[6] == 0x22) current->report_length = (uint16_t)(d[7] | d[8] << 8);
        } else if (d[1] == 5 && d[0] >= 7 && current) {
            unsigned type = d[3] & 3;
            uint16_t packet = (uint16_t)((d[4] | d[5] << 8) & 0x7ff);
            struct usb_endpoint e = {(uint8_t)id, (uint8_t)endpoint_dci(d[2]), packet};
            if (current->class == USB_CLASS_HID && type == 3 && (d[2] & 0x80) && !current->in.dci) {
                current->in = e;
                current->in_interval = d[6];
            } else if (current->class == USB_CLASS_STORAGE && type == 2) {
                if ((d[2] & 0x80) && !current->in.dci) current->in = e;
                if (!(d[2] & 0x80) && !current->out.dci) current->out = e;
            }
        }
    }
    unsigned kept = 0;
    for (unsigned i = 0; i < count; ++i)
        if (out[i].in.dci && (out[i].class == USB_CLASS_HID || out[i].out.dci))
            out[kept++] = out[i];
    return kept;
}

static struct transfer *add_transfer(struct device *d, unsigned dci) {
    for (unsigned i = 1; i < RINGS; ++i)
        if (!d->transfers[i].dci) {
            struct transfer *t = &d->transfers[i];
            *t = (struct transfer){.dci = (uint8_t)dci};
            ring_init(&t->ring, d->rings[i]);
            return t;
        }
    return 0;
}

/* The device on a root port: reset (a USB 2 port; a USB 3 one trains by
 * itself) and given its recovery time, addressed, its configuration read and
 * set, and the endpoints of its interfaces configured for their drivers, as
 * many interfaces as its rings take. The port's changes are cleared on the
 * way, so that it reports the next one. */
static bool enumerate(struct controller *c, unsigned port) {
    uint32_t status = port_acknowledge(c, port);
    if (!(status & CCS)) return false;
    if (c->port_protocol[port] != 3) {
        port_write(c, port, (status & PORT_KEEP) | PR);
        bool reset = false;
        for (uint64_t deadline = time_us() + 500000; !reset;) {
            reset = port_read(c, port) & PRC;
            if (!reset && expired(deadline)) break;
        }
        status = port_acknowledge(c, port);
        if (!reset) return false;
        delay_us(RESET_RECOVERY_US);
    } else {
        for (uint64_t deadline = time_us() + 500000; !(port_read(c, port) & PED);)
            if (expired(deadline)) break;
        status = port_acknowledge(c, port);
    }
    if (!(status & PED)) return false;
    uint8_t speed = (status >> 10) & 15, slot = 0;
    if (!command(c, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) || !slot || slot > c->slot_count)
        return false;
    unsigned id = 1;
    while (id <= DEVICES && devices[id].used) ++id;
    if (id > DEVICES || c->device[slot]) {
        (void)command(c, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)slot << 24, 0);
        return false;
    }
    struct device *d = &devices[id];
    memset(d, 0, sizeof(*d));
    d->used = true;
    d->host = c;
    d->generation = ++generations;
    d->slot = slot;
    d->port = (uint8_t)port;
    d->speed = speed;
    c->device[slot] = (uint8_t)id;
    struct transfer *ep0 = &d->transfers[0];
    ep0->dci = 1;
    ring_init(&ep0->ring, d->rings[0]);
    /* SuperSpeed and SuperSpeedPlus (speeds 4 and up, on a USB 3 port): 512. */
    uint16_t packet = c->port_protocol[port] == 3 || speed >= 4 ? 512 : speed == 3 ? 64 : 8;
    uint32_t *control = context(c, d->input, 0), *slot_context = context(c, d->input, 1);
    control[1] = 3; /* the slot and endpoint 0 */
    slot_context[0] = (uint32_t)speed << 20 | 1u << 27;
    slot_context[1] = (uint32_t)port << 16;
    endpoint_context(d, 1, 4, packet, 0, ep0);
    c->dcbaa[slot] = (uintptr_t)d->output;
    if (!command(c, (uintptr_t)d->input, TRB_TYPE(TRB_ADDRESS_DEVICE) | (uint32_t)slot << 24, 0))
        goto failed;
    delay_us(ADDRESS_RECOVERY_US);
    /* A full-speed device's endpoint 0 takes the packet size it states. */
    if (!usb_control(id, 0x80, 6, 0x0100, 0, descriptor, 8, 500000)) goto failed;
    if (speed < 3 && descriptor[7] && descriptor[7] != packet) {
        packet = descriptor[7];
        memset(d->input, 0, sizeof(d->input));
        control[1] = 2;
        endpoint_context(d, 1, 4, packet, 0, ep0);
        if (!command(c, (uintptr_t)d->input, TRB_TYPE(TRB_EVALUATE_CONTEXT) | (uint32_t)slot << 24,
                     0))
            goto failed;
    }
    if (!usb_control(id, 0x80, 6, 0x0200, 0, descriptor, 9, 500000)) goto failed;
    unsigned total = descriptor[2] | descriptor[3] << 8;
    if (total < 9 || total > CONFIGURATION ||
        !usb_control(id, 0x80, 6, 0x0200, 0, descriptor, (uint16_t)total, 500000))
        goto failed;
    struct interface interfaces[4];
    unsigned found = parse_configuration(descriptor, total, id, interfaces, 4);
    if (!usb_control(id, 0x00, 9, descriptor[5], 0, 0, 0, 500000)) goto failed;
    memset(d->input, 0, sizeof(d->input));
    unsigned last = 1, rings = RINGS - 1, kept = 0;
    for (unsigned i = 0; i < found; ++i) {
        struct interface *f = &interfaces[i];
        unsigned needed = f->out.dci ? 2 : 1;
        if (needed > rings) continue; /* no rings left for it: left alone */
        rings -= needed;
        struct usb_endpoint *ends[2] = {&f->in, f->out.dci ? &f->out : 0};
        for (unsigned j = 0; j < 2; ++j) {
            struct usb_endpoint *e = ends[j];
            if (!e) continue;
            struct transfer *t = add_transfer(d, e->dci);
            unsigned type = f->class == USB_CLASS_HID ? 7 : (e->dci & 1) ? 6 : 2;
            endpoint_context(d, e->dci, type, e->max_packet,
                             f->class == USB_CLASS_HID ? interval(speed, f->in_interval) : 0, t);
            control[1] |= 1u << e->dci;
            if (e->dci > last) last = e->dci;
        }
        interfaces[kept++] = *f;
    }
    if (!kept) return true; /* nothing SOS2 drives */
    control[1] |= 1;
    memcpy(slot_context, context(c, d->output, 0), c->context_size);
    slot_context[0] = (slot_context[0] & ~(31u << 27)) | last << 27;
    if (!command(c, (uintptr_t)d->input, TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | (uint32_t)slot << 24,
                 0))
        goto failed;
    for (unsigned i = 0; i < kept; ++i) {
        struct interface *f = &interfaces[i];
        if (f->class == USB_CLASS_HID)
            (void)usb_hid_attach(id, f->number, f->subclass == 1 ? f->protocol : 0, &f->in,
                                 f->report_length);
        else (void)usb_storage_attach(id, f->number, &f->in, &f->out);
    }
    return true;
failed:
    release(id);
    return false;
}

/* The controllers' windows, mapped with the kernel's devices, and at once,
 * since the stick they hold is opened before them (system_update_init). */
void xhci_probe(struct platform_devices *d) {
    controller_count = 0;
    for (unsigned i = 0; controller_count < CONTROLLERS; ++i) {
        const struct pci_function *f = pci_find(0x0c0330, 0xffffff, i);
        if (!f) break;
        if (pci_map(d, f, 0) && machine_early_map(f->bar[0], f->bar_size[0]))
            controllers[controller_count++].pci = f;
    }
}

/* Ports by protocol (USB 2 or 3) from the supported-protocol capabilities,
 * after the firmware hands the controller over (USB legacy support). */
static void extended_capabilities(struct controller *c, uint32_t parameters) {
    uintptr_t first = parameters >> 16 ? c->capabilities + ((parameters >> 16) << 2) : 0;
    for (uintptr_t at = first, next; at; at = next) {
        uint32_t header = mmio_read32(at);
        next = (header >> 8) & 0xff ? at + (((header >> 8) & 0xff) << 2) : 0;
        if ((header & 0xff) == 1) {
            mmio_write32(at, header | 1u << 24);
            for (uint64_t deadline = time_us() + 1000000; mmio_read32(at) & 1u << 16;)
                if (time_us() > deadline) break;
            mmio_write32(at + 4, 0xe0000000); /* its SMIs off, their status cleared */
        } else if ((header & 0xff) == 2) {
            uint32_t ports = mmio_read32(at + 8);
            unsigned first = ports & 0xff, count = (ports >> 8) & 0xff;
            for (unsigned p = first; p < first + count && p <= PORTS; ++p)
                c->port_protocol[p] = (uint8_t)(header >> 24);
        }
    }
}

/* Intel's 7 and 8 series chipsets share their USB 2 ports between EHCI and
 * xHCI and keep them on EHCI until the system takes them over, as Linux's
 * usb_enable_intel_xhci_ports does: the ports the firmware allows, USB 3's
 * terminations on (USB3_PSSEN from USB3PRM) and USB 2's routed here (XUSB2PR
 * from USB2PRM). */
static void intel_ports(const struct pci_function *f) {
    if (f->vendor != 0x8086 || (f->id != 0x1e31 && f->id != 0x8c31 && f->id != 0x9c31)) return;
    pci_write32(f, 0xd8, pci_read32(f, 0xdc));
    pci_write32(f, 0xd0, pci_read32(f, 0xd4));
}

static bool start(struct controller *c) {
    const struct pci_function *f = c->pci;
    pci_enable(f);
    intel_ports(f);
    c->capabilities = (uintptr_t)f->bar[0];
    c->operational = c->capabilities + mmio_read8(c->capabilities + CAPLENGTH);
    c->runtime = c->capabilities + (mmio_read32(c->capabilities + RTSOFF) & ~0x1fu);
    c->doorbells = c->capabilities + (mmio_read32(c->capabilities + DBOFF) & ~3u);
    uint32_t structural = mmio_read32(c->capabilities + HCSPARAMS1),
             parameters = mmio_read32(c->capabilities + HCCPARAMS1);
    c->context_size = parameters & 4 ? 64 : 32;
    c->port_count = structural >> 24;
    c->slot_count = structural & 0xff;
    if (c->port_count > PORTS) c->port_count = PORTS;
    if (c->slot_count > SLOTS) c->slot_count = SLOTS;
    extended_capabilities(c, parameters);
    op_write(c, USBCMD, op_read(c, USBCMD) & ~(uint32_t)RUN);
    for (uint64_t deadline = time_us() + 100000; !(op_read(c, USBSTS) & HALTED);)
        if (time_us() > deadline) return false;
    op_write(c, USBCMD, HCRST);
    delay_us(1000); /* Intel's controllers want a millisecond before the next access */
    for (uint64_t deadline = time_us() + 1000000;
         (op_read(c, USBCMD) & HCRST) || (op_read(c, USBSTS) & NOT_READY);)
        if (time_us() > deadline) return false;
    uint32_t sizing = mmio_read32(c->capabilities + HCSPARAMS2);
    unsigned pads = (sizing >> 27 & 31) | (sizing >> 21 & 31) << 5;
    if (pads > SCRATCHPADS || !(op_read(c, PAGESIZE) & 1)) return false;
    memset(c->dcbaa, 0, sizeof(c->dcbaa));
    memset(c->device, 0, sizeof(c->device));
    uint8_t (*pages)[4096] = scratchpads[c - controllers];
    for (unsigned i = 0; i < pads; ++i) {
        memset(pages[i], 0, sizeof(pages[i]));
        c->scratchpad_array[i] = (uintptr_t)pages[i];
    }
    if (pads) c->dcbaa[0] = (uintptr_t)c->scratchpad_array;
    op_write(c, CONFIG, c->slot_count);
    op_write64(c, DCBAAP, (uintptr_t)c->dcbaa);
    ring_init(&c->commands, c->command_trbs);
    op_write64(c, CRCR, (uintptr_t)c->command_trbs | 1);
    memset(c->events, 0, sizeof(c->events));
    c->event_dequeue = 0;
    c->event_cycle = 1;
    c->segment_table[0].base = (uintptr_t)c->events;
    c->segment_table[0].size = RING;
    mmio_write32(c->runtime + ERSTSZ, 1);
    rt_write64(c, ERDP, (uintptr_t)c->events);
    rt_write64(c, ERSTBA, (uintptr_t)c->segment_table);
    mmio_write32(c->runtime + IMAN, 1); /* no interrupts: polled */
    op_write(c, USBCMD, RUN);
    for (uint64_t deadline = time_us() + 100000; op_read(c, USBSTS) & HALTED;)
        if (time_us() > deadline) return false;
    c->running = true;
    for (unsigned port = 1; port <= c->port_count; ++port) {
        uint32_t status = port_read(c, port);
        if (!(status & PP)) port_write(c, port, (status & PORT_KEEP) | PP);
    }
    return true;
}

bool xhci_start(void) {
    if (started) return running;
    started = true;
    for (unsigned i = 0; i < controller_count; ++i) running |= start(&controllers[i]);
    if (!running) return false;
    delay_us(20000);
    enumerating = true;
    for (unsigned i = 0; i < controller_count; ++i) {
        struct controller *c = &controllers[i];
        if (!c->running) continue;
        for (unsigned port = 1; port <= c->port_count; ++port) {
            c->port_changed[port] = false;
            if (port_read(c, port) & CCS) (void)enumerate(c, port);
        }
    }
    enumerating = false;
    return true;
}

/* A port's device, if it has one. */
static unsigned device_on(const struct controller *c, unsigned port) {
    for (unsigned id = 1; id <= DEVICES; ++id)
        if (devices[id].used && devices[id].host == c && devices[id].port == port) return id;
    return 0;
}

/* Ports whose connection changed: a device gone (or gone and back) is
 * released, and one there once its connection has held a debounce,
 * enumerated. A port's other changes, such as its own reset's, are only
 * cleared, so a device enumerated without success waits for its next
 * connection. A command waited for on the service worker defers them. */
void xhci_service(void) {
    if (!running || enumerating) return;
    xhci_poll();
    if (command_waiting) return;
    enumerating = true;
    for (unsigned i = 0; i < controller_count; ++i) {
        struct controller *c = &controllers[i];
        if (!c->running) continue;
        for (unsigned port = 1; port <= c->port_count; ++port) {
            if (!c->port_changed[port]) continue;
            c->port_changed[port] = false;
            uint32_t status = port_acknowledge(c, port);
            if (!(status & CSC)) continue;
            unsigned id = device_on(c, port);
            if (id) release(id);
            if (!(status & CCS)) continue;
            delay_us(DEBOUNCE_US);
            if (port_read(c, port) & CCS) (void)enumerate(c, port);
        }
    }
    enumerating = false;
}
