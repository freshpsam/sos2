#include "nexus5.h"
#include "platform.h"

/* The Nexus 5's USB port as a fastboot device (fastboot.c), for the computer
 * it is plugged into: MSM8974's ChipIdea high-speed controller at
 * 0xF9A55000, which the stock bootloader's own fastboot drives (CAF LK's
 * hsusb.c: target_usb_controller picks "ci" for this chip, platform 126, and
 * HHZ20h logs hsusb.c's "USB init ept"). The bootloader resets the
 * controller before it starts the kernel (udc_stop) but leaves its clocks
 * and ULPI PHY set up, and on MSM8974 the PHY senses VBUS itself
 * (target_needs_vbus_mimic is 0), so starting it is LK's udc_init and
 * udc_start without the clocks: a reset, the ULPI PHY selected, the AHB
 * settings, the queue heads, device mode, then run, which connects the D+
 * pull-up the host sees.
 * Polled: control transfers on endpoint 0 and one bulk endpoint each way
 * (512-byte packets at high speed, 64 at full speed), each transfer one
 * descriptor of at most 16 KiB. Queue heads, descriptors and buffers lie in
 * 64 KiB of RAM at 0x02600000 that boot.dts reserves and that is never
 * cached: unmapped (Device) under the resource decode's mapping
 * (arch/arm/boot_cache.c), a device mapping afterwards (arch/arm/machine.c).
 * So the link runs through the whole start, and their addresses are bus
 * addresses needing no cache maintenance.
 * The USB IDs are the generic fastboot ones the tablet's link uses, with the
 * serial number "nexus5". */
enum {
    USB = 0xf9a55000,
    SBUSCFG = 0x090,
    AHB_MODE = 0x098,
    USBCMD = 0x140,
    USBSTS = 0x144,
    USBINTR = 0x148,
    DEVICEADDR = 0x154,
    ENDPOINTLISTADDR = 0x158,
    PORTSC = 0x184,
    USBMODE = 0x1a8,
    ENDPTSETUPSTAT = 0x1ac,
    ENDPTPRIME = 0x1b0,
    ENDPTFLUSH = 0x1b4,
    ENDPTSTAT = 0x1b8,
    ENDPTCOMPLETE = 0x1bc,
    ENDPTCTRL0 = 0x1c0,
    ENDPTCTRL1 = 0x1c4,
    CMD_RUN = 1,
    CMD_RESET = 2,
    CMD_ITC_8 = 8u << 16, /* interrupt threshold, as LK sets it */
    PORTSC_ULPI = 0x80000000u,
    MODE_DEVICE = 2,
    STS_URI = 1u << 6, /* bus reset */
    STS_PCI = 1u << 2, /* port change: the speed is known */
    CTRL_TXE = 1u << 23,
    CTRL_TXR = 1u << 22,
    CTRL_TX_BULK = 2u << 18,
    CTRL_TXS = 1u << 16,
    CTRL_RXE = 1u << 7,
    CTRL_RXR = 1u << 6,
    CTRL_RX_BULK = 2u << 2,
    CTRL_RXS = 1u << 0,
    HEAD_IOS = 1u << 15, /* interrupt on setup: endpoint 0 OUT */
    HEAD_NO_ZLT = 1u << 29,
    TOKEN_ACTIVE = 1u << 7,
    TOKEN_ERRORS = (1u << 6) | (1u << 5) | (1u << 3), /* halted, buffer, transaction */
    TOKEN_IOC = 1u << 15,
    TERMINATE = 1,
    /* GCC branch clocks of the controller; bit 31 is CLK_OFF (gcc-msm8974.c). */
    GCC_USB_HS_SYSTEM_CBCR = 0xfc400484,
    GCC_USB_HS_AHB_CBCR = 0xfc400488,
    CLOCK_OFF = 1u << 31,
    /* Endpoint indices: 2 * number + direction (1 = IN). */
    EP0_OUT = 0,
    EP0_IN = 1,
    EP1_OUT = 2,
    EP1_IN = 3,
    CHUNK = 16384,
    COMMAND = 512,
    RESET_US = 200000,
    TRANSFER_US = 2000000
};

struct queue_head {
    uint32_t config, current, next, token, page[5], reserved;
    uint8_t setup[8];
    uint32_t pad[4];
};

_Static_assert(sizeof(struct queue_head) == 64, "ChipIdea queue head");

struct transfer {
    uint32_t next, token, page[5], reserved;
    uint32_t pad[8]; /* a cache line to itself */
};

_Static_assert(sizeof(struct transfer) == 64, "ChipIdea transfer descriptor");

struct dma {
    struct queue_head heads[4];   /* ENDPOINTLISTADDR: 2 KiB aligned */
    struct transfer transfers[4]; /* one per endpoint direction */
    uint8_t control[256];
    uint8_t command[COMMAND];
};

enum { DMA_AREA = 0x02600000, CHUNK_AREA = DMA_AREA + 0x4000 };

_Static_assert(sizeof(struct dma) <= 0x4000 && CHUNK <= 0x10000 - 0x4000, "USB DMA area");
#define dma   (*(struct dma *)DMA_AREA)
#define chunk ((uint8_t *)CHUNK_AREA)

static bool started, connected, configured, high_speed, receiving;

static uint32_t reg(unsigned offset) { return mmio_read32(USB + offset); }

static void set(unsigned offset, uint32_t value) {
    barrier();
    mmio_write32(USB + offset, value);
    barrier();
}

static uint32_t bit(unsigned index) {
    return index & 1 ? 1u << (16 + index / 2) : 1u << (index / 2);
}

static volatile struct queue_head *head(unsigned index) { return &dma.heads[index]; }

static volatile struct transfer *transfer(unsigned index) { return &dma.transfers[index]; }

/* One descriptor for length bytes at data (at most 16 KiB from a 4 KiB
 * boundary: its five page pointers cover any 16 KiB). */
static void prime(unsigned index, const void *data, uint32_t length) {
    volatile struct transfer *t = transfer(index);
    uintptr_t at = (uintptr_t)data;
    t->next = TERMINATE;
    t->token = length << 16 | TOKEN_IOC | TOKEN_ACTIVE;
    t->page[0] = (uint32_t)at;
    for (unsigned i = 1; i < 5; ++i) t->page[i] = (uint32_t)((at & ~4095u) + 4096u * i);
    head(index)->next = (uint32_t)(uintptr_t)t;
    head(index)->token = 0;
    set(ENDPTPRIME, bit(index));
}

/* Whether the endpoint's transfer is still the controller's: its descriptor
 * active, or not yet retired. The token is written back first, and a prime
 * made before the endpoint's ENDPTSTAT bit clears is lost (on the phone, one
 * reply in about 60 never went out, 2026-11-21). */
static bool busy(unsigned index) {
    return (transfer(index)->token & TOKEN_ACTIVE) || (reg(ENDPTSTAT) & bit(index));
}

/* The transfer's end: false on an error, a bus reset or the timeout, which
 * flushes the endpoint. done gets the bytes moved. */
static bool finish(unsigned index, uint32_t length, uint32_t *done, uint64_t timeout_us) {
    volatile struct transfer *t = transfer(index);
    uint64_t start = time_us();
    while (busy(index)) {
        if ((reg(USBSTS) & STS_URI) || time_us() - start >= timeout_us) {
            set(ENDPTFLUSH, bit(index));
            return false;
        }
    }
    set(ENDPTCOMPLETE, bit(index));
    if (t->token & TOKEN_ERRORS) return false;
    if (done) *done = length - (t->token >> 16 & 0x7fff);
    return true;
}

static void copy_out(uint8_t *to, const void *from, uint32_t length) {
    const uint8_t *p = from;
    volatile uint8_t *q = to;
    for (uint32_t i = 0; i < length; ++i) q[i] = p[i];
}

static bool control_in(const void *data, unsigned length) {
    copy_out(dma.control, data, length);
    prime(EP0_IN, dma.control, length);
    if (!finish(EP0_IN, length, 0, RESET_US)) return false;
    prime(EP0_OUT, 0, 0); /* status stage */
    return finish(EP0_OUT, 0, 0, RESET_US);
}

static bool control_ack(void) {
    prime(EP0_IN, 0, 0);
    return finish(EP0_IN, 0, 0, RESET_US);
}

static void stall(void) { set(ENDPTCTRL0, CTRL_TXS | CTRL_RXS); }

static unsigned string(uint8_t *out, const char *text) {
    unsigned n = 2;
    for (; *text && n < 64; ++text, n += 2) {
        out[n] = (uint8_t)*text;
        out[n + 1] = 0;
    }
    out[0] = (uint8_t)n;
    out[1] = 3;
    return n;
}

static unsigned descriptor(uint16_t value, uint8_t *out) {
    static const uint8_t device[18] = {18, 1, 0x00, 0x02, 0, 0, 0, 64, 0xd1, 0x18, 0x0d, 0xd0,
                                       0x00, 0x01, 1, 2, 3, 1};
    unsigned type = value >> 8, index = value & 255;
    if (type == 1) {
        memcpy(out, device, sizeof(device));
        return sizeof(device);
    }
    if (type == 2) {
        unsigned packet = high_speed ? 512 : 64;
        const uint8_t config[32] = {9, 2, 32, 0, 1, 1, 0, 0x80, 0xfa,
                                    /* the fastboot interface */
                                    9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 0,
                                    /* bulk IN 1, bulk OUT 1 */
                                    7, 5, 0x81, 2, (uint8_t)packet, (uint8_t)(packet >> 8), 0,
                                    7, 5, 0x01, 2, (uint8_t)packet, (uint8_t)(packet >> 8), 0};
        memcpy(out, config, sizeof(config));
        return sizeof(config);
    }
    if (type == 3) {
        if (index == 0) {
            static const uint8_t languages[4] = {4, 3, 0x09, 0x04};
            memcpy(out, languages, sizeof(languages));
            return sizeof(languages);
        }
        static const char *const strings[] = {"SOS2", "Nexus 5", "nexus5"};
        if (index <= 3) return string(out, strings[index - 1]);
    }
    return 0;
}

static void receive_command(void) {
    prime(EP1_OUT, dma.command, COMMAND);
    receiving = true;
}

static void configure(bool on) {
    configured = on;
    receiving = false;
    if (!on) {
        set(ENDPTCTRL1, 0);
        return;
    }
    unsigned packet = high_speed ? 512 : 64;
    head(EP1_OUT)->config = packet << 16 | HEAD_NO_ZLT;
    head(EP1_IN)->config = packet << 16 | HEAD_NO_ZLT;
    set(ENDPTCTRL1, CTRL_TXE | CTRL_TXR | CTRL_TX_BULK | CTRL_RXE | CTRL_RXR | CTRL_RX_BULK);
    receive_command();
}

static void setup(void) {
    uint8_t s[8];
    for (unsigned i = 0; i < 8; ++i) s[i] = head(EP0_OUT)->setup[i];
    set(ENDPTSETUPSTAT, 1);
    unsigned type = s[0], request = s[1];
    uint16_t value = (uint16_t)(s[2] | s[3] << 8), length = (uint16_t)(s[6] | s[7] << 8);
    if (type == 0x80 && request == 6) { /* GET_DESCRIPTOR */
        uint8_t data[64];
        unsigned n = descriptor(value, data);
        if (!n) {
            stall();
            return;
        }
        (void)control_in(data, n < length ? n : length);
    } else if (type == 0x80 && request == 0 && length == 2) { /* GET_STATUS */
        static const uint8_t zero[2];
        (void)control_in(zero, 2);
    } else if (type == 0x80 && request == 8 && length == 1) { /* GET_CONFIGURATION */
        uint8_t current = configured;
        (void)control_in(&current, 1);
    } else if (type == 0x00 && request == 5) { /* SET_ADDRESS, after the status stage */
        set(DEVICEADDR, (uint32_t)value << 25 | 1u << 24);
        (void)control_ack();
    } else if (type == 0x00 && request == 9) { /* SET_CONFIGURATION */
        configure(value == 1);
        (void)control_ack();
    } else if (type == 0x02 && request == 1 && !value) { /* CLEAR_FEATURE(HALT) */
        if ((s[4] & 15) == 1) configure(configured);
        (void)control_ack();
    } else {
        stall();
    }
}

const char *nexus5_usb_init(void) {
    if (started) return 0;
    if ((mmio_read32(GCC_USB_HS_SYSTEM_CBCR) & CLOCK_OFF) ||
        (mmio_read32(GCC_USB_HS_AHB_CBCR) & CLOCK_OFF))
        return "USB clocks off";
    set(USBCMD, CMD_ITC_8 | CMD_RESET);
    for (uint64_t at = time_us(); reg(USBCMD) & CMD_RESET;)
        if (time_us() - at >= RESET_US) return "USB controller reset timed out";
    set(PORTSC, PORTSC_ULPI);
    set(SBUSCFG, 0);
    set(AHB_MODE, 0x08);
    for (unsigned i = 0; i < sizeof(dma) / 4; ++i) ((volatile uint32_t *)DMA_AREA)[i] = 0;
    dma.heads[EP0_OUT].config = 64u << 16 | HEAD_IOS | HEAD_NO_ZLT;
    dma.heads[EP0_IN].config = 64u << 16 | HEAD_NO_ZLT;
    for (unsigned i = 0; i < 4; ++i) dma.heads[i].next = TERMINATE;
    set(ENDPOINTLISTADDR, (uint32_t)(uintptr_t)dma.heads);
    set(USBMODE, MODE_DEVICE);
    set(ENDPTFLUSH, UINT32_MAX);
    set(USBINTR, 0); /* polled */
    started = true;
    return 0;
}

bool nexus5_usb_rescue(void) {
    if (!started && nexus5_usb_init()) return false;
    nexus5_usb_connect();
    return true;
}

void nexus5_usb_connect(void) {
    if (!started || connected) return;
    set(USBCMD, reg(USBCMD) | CMD_ITC_8 | CMD_RUN);
    connected = true;
}

/* A bus reset: endpoint 0 alone, at address 0. */
static void bus_reset(void) {
    set(ENDPTSETUPSTAT, reg(ENDPTSETUPSTAT));
    set(ENDPTCOMPLETE, reg(ENDPTCOMPLETE));
    set(ENDPTFLUSH, UINT32_MAX);
    set(DEVICEADDR, 0);
    configure(false);
}

void nexus5_usb_poll(void) {
    if (!connected) return;
    uint32_t status = reg(USBSTS);
    set(USBSTS, status);
    if (status & STS_URI) bus_reset();
    if (status & STS_PCI) high_speed = (reg(PORTSC) >> 26 & 3) == 2;
    if (reg(ENDPTSETUPSTAT) & 1) setup();
    if (!configured || !receiving || busy(EP1_OUT)) return;
    receiving = false;
    uint32_t length = 0;
    bool ok = finish(EP1_OUT, COMMAND, &length, 0);
    if (ok && length) {
        char command[COMMAND + 1];
        for (uint32_t i = 0; i < length; ++i)
            command[i] = (char)((volatile uint8_t *)dma.command)[i];
        command[length] = 0;
        nexus5_fastboot(command, length);
    }
    if (configured && !receiving) receive_command();
}

/* Sends data to the host in transfers of at most 16 KiB, each copied into
 * chunk, the uncached buffer the controller reads. */
bool nexus5_usb_send(const void *data, uint32_t length) {
    if (!configured) return false;
    const uint8_t *p = data;
    do {
        uint32_t n = length < CHUNK ? length : CHUNK, done;
        copy_out(chunk, p, n);
        prime(EP1_IN, chunk, n);
        if (!finish(EP1_IN, n, &done, TRANSFER_US) || done != n) return false;
        p += n;
        length -= n;
    } while (length);
    return true;
}
