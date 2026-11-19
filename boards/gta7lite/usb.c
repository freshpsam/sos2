#include "gta7lite.h"
#include "platform.h"

/* MT6765's USB 2.0 port as a fastboot device (fastboot.c): the tablet's link
 * to the computer it charges from. Samsung's mt6765.dts usb0@11200000 is a
 * Mentor MUSB controller; its PHY's registers start 0x800 into usb0's second
 * window. Polled programmed I/O: the control endpoint and one bulk endpoint
 * each way, 512-byte packets at high speed (64 at full speed). The OUT FIFO
 * is double-buffered, so a download streams while the last packet is read;
 * the IN FIFO is single-buffered, so a clear TxPktRdy means the host has
 * everything sent.
 *
 * Bring-up follows MediaTek's MT6765 driver (drivers/misc/mediatek/usb20 in
 * https://github.com/gta7lite/android_kernel_samsung_mt6765): the MT6357's
 * VUSB33 (mt_usb_init), the controller's clocks (usb_enable_clock with
 * clk-mt6765.c: infracfg's ICUSB gate, topckgen's usb_top_sel on
 * univpll3_d4), the PHY's power-on and recovery sequences with device mode
 * forced (usb_phy_poweron, usb_phy_recover, set_usb_phy_mode), then the
 * controller's soft reset and HSENAB (musb_platform_reset, musb_start).
 * SOFTCONN, the D+ pull-up the host sees, waits for gta7lite_usb_connect: a
 * host whose new device does not answer for seconds gives up on the port.
 * MediaTek's controller clears an interrupt flag when it is written back
 * (generic_interrupt). Control transfers follow Linux's musb_gadget_ep0.c:
 * SET_ADDRESS's address takes effect once its status stage ends, so that
 * request is serviced until then. The USB IDs are the generic fastboot
 * ones lk2nd uses, which Android's udev rules give the desktop user. */

enum {
    /* topckgen's usb_top_sel: CLK_CFG_5 bit 16 (parent 1, univpll3_d4),
     * gate bit 23, update bit 22 of CLK_CFG_UPDATE. */
    TOPCKGEN = 0x10000000,
    CLK_CFG_UPDATE = 0x004,
    CLK_CFG_5 = 0x090,
    CLK_CFG_5_SET = 0x094,
    CLK_CFG_5_CLR = 0x098,
    USB_TOP_UNIVPLL3_D4 = 1u << 16,
    USB_TOP_UPDATE = 1u << 22,
    USB_TOP_OFF = 1u << 23,
    /* infracfg_ao's MODULE_SW_CG_0: ICUSB is bit 8; a set status bit is a
     * gated clock. */
    INFRACFG = 0x10001000,
    CG0_CLR = 0x084,
    CG0_STA = 0x090,
    ICUSB = 1u << 8,
    /* The PHY (usb20_phy.c's USBPHY_* offsets). */
    PHY = 0x11cc0800,
    PHY_ACR6 = 0x18, /* [31:24] PHY_REV6, 23 BC11_SW_EN, 20 OTG_VBUSCMP_EN, [7:4] DISCTH */
    PHY_ACR3 = 0x1c, /* 12 PUPD_BIST_EN */
    PHY_ACR4 = 0x20, /* 18 dp_100k_mode, 17 DM_100K_EN, 16 DP_100K_EN, 9 gpio_ctl, 8 gpio_mode */
    PHY_DTM0 = 0x68, /* the UTMI signals the PHY can force */
    PHY_DTM1 = 0x6c, /* 16 RG_UART_EN; [13:8] forced, [5:0] their values */
    /* The controller. */
    MUSB = 0x11200000,
    FADDR = 0x00,
    POWER = 0x01,
    INTRTX = 0x02,
    INTRRX = 0x04,
    INTRTXE = 0x06,
    INTRRXE = 0x08,
    INTRUSB = 0x0a,
    INTRUSBE = 0x0b,
    INDEX = 0x0e,
    TXMAXP = 0x10, /* 0x10-0x1f: the endpoint INDEX selects */
    CSR0 = 0x12,
    TXCSR = 0x12,
    RXMAXP = 0x14,
    RXCSR = 0x16,
    COUNT0 = 0x18,
    RXCOUNT = 0x18,
    FIFO = 0x20, /* then 4 bytes per endpoint */
    TXFIFOSZ = 0x62,
    RXFIFOSZ = 0x63,
    TXFIFOADD = 0x64,
    RXFIFOADD = 0x66,
    EPINFO = 0x78,
    SWRST = 0x74,
    L1INTM = 0xa4,
    POWER_HSMODE = 0x10,
    POWER_HSENAB = 0x20,
    POWER_SOFTCONN = 0x40,
    INTR_SUSPEND = 0x01,
    INTR_RESUME = 0x02,
    INTR_RESET = 0x04,
    CSR0_RXPKTRDY = 0x0001,
    CSR0_TXPKTRDY = 0x0002,
    CSR0_SENTSTALL = 0x0004,
    CSR0_DATAEND = 0x0008,
    CSR0_SETUPEND = 0x0010,
    CSR0_SENDSTALL = 0x0020,
    CSR0_SVDRXPKTRDY = 0x0040,
    CSR0_SVDSETUPEND = 0x0080,
    TX_PKTRDY = 0x0001,
    TX_FIFONOTEMPTY = 0x0002,
    TX_UNDERRUN = 0x0004,
    TX_FLUSHFIFO = 0x0008,
    TX_CLRDATATOG = 0x0040,
    TX_MODE = 0x2000,
    RX_PKTRDY = 0x0001,
    RX_OVERRUN = 0x0004,
    RX_FLUSHFIFO = 0x0010,
    RX_SENTSTALL = 0x0040,
    RX_CLRDATATOG = 0x0080,
    SWRST_DISUSBRESET = 1 << 0,
    SWRST_SWRST = 1 << 1,
    /* FIFO RAM: endpoint 0's fixed 64 bytes, then 512 bytes (size code 6)
     * for bulk IN and twice that for bulk OUT's two buffers. */
    BULK = 1,
    FIFO_512 = 6,
    FIFO_DOUBLE = 0x10,
    TX_FIFO_AT = 64,
    RX_FIFO_AT = 64 + 512,
    EP0_PACKET = 64,
    /* Waits: a control transfer's next stage; the host taking a bulk
     * packet; the host taking the last response before a restart. A
     * response left unread longer than STALE_US has no reader: a fastboot
     * that is still running reads each one at once. */
    CONTROL_US = 50000,
    PACKET_US = 2000000,
    DRAIN_US = 100000,
    STALE_US = 500000
};

/* Standard requests (USB 2.0 chapter 9). */
enum {
    GET_STATUS = 0,
    CLEAR_FEATURE = 1,
    SET_ADDRESS = 5,
    GET_DESCRIPTOR = 6,
    GET_CONFIGURATION = 8,
    SET_CONFIGURATION = 9,
    GET_INTERFACE = 10,
    SET_INTERFACE = 11
};

/* 18d1:d00d, fastboot's generic IDs; strings 1-3: manufacturer, product,
 * serial number. */
static const unsigned char device_descriptor[18] = {18,   1,    0x00, 0x02, 0, 0, 0, EP0_PACKET, 0xd1,
                                                    0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1};
static const unsigned char qualifier_descriptor[10] = {10, 6, 0x00, 0x02, 0, 0, 0, EP0_PACKET, 1, 0};
static const char *const strings[] = {"SOS2", "SOS2 tablet", "gta7lite"};

static struct {
    const char *error;
    bool started, connected, configured, polling;
    uint16_t packet; /* the bulk endpoints' packet size */
    uint64_t queued_at; /* when the last bulk IN packet was queued */
    unsigned out_at, out_end; /* a bulk OUT packet's bytes not yet read */
    unsigned char out[512] __attribute__((aligned(4)));
    char command[512 + 1];
    unsigned char control[EP0_PACKET] __attribute__((aligned(4)));
} usb;

static uint8_t read8(unsigned offset) { return mmio_read8(MUSB + offset); }

static uint16_t read16(unsigned offset) { return mmio_read16(MUSB + offset); }

static void write8(unsigned offset, uint8_t value) { mmio_write8(MUSB + offset, value); }

static void write16(unsigned offset, uint16_t value) { mmio_write16(MUSB + offset, value); }

static void phy(unsigned offset, uint32_t clear, uint32_t set) {
    mmio_write32(PHY + offset, (mmio_read32(PHY + offset) & ~clear) | set);
}


/* The FIFO takes and gives four bytes per 32-bit access, in order. */
static void fifo_write(unsigned endpoint, const unsigned char *data, unsigned n) {
    uintptr_t fifo = MUSB + FIFO + 4 * endpoint;
    unsigned i = 0;
    for (; i + 4 <= n; i += 4)
        mmio_write32(fifo, (uint32_t)data[i] | (uint32_t)data[i + 1] << 8 |
                               (uint32_t)data[i + 2] << 16 | (uint32_t)data[i + 3] << 24);
    for (; i < n; ++i) mmio_write8(fifo, data[i]);
}

static void fifo_read(unsigned endpoint, unsigned char *out, unsigned n) {
    uintptr_t fifo = MUSB + FIFO + 4 * endpoint;
    unsigned i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t word = mmio_read32(fifo);
        out[i] = (unsigned char)word;
        out[i + 1] = (unsigned char)(word >> 8);
        out[i + 2] = (unsigned char)(word >> 16);
        out[i + 3] = (unsigned char)(word >> 24);
    }
    for (; i < n; ++i) out[i] = mmio_read8(fifo);
}

static void fifo_sizes(void) {
    write8(INDEX, BULK);
    write8(TXFIFOSZ, FIFO_512);
    write16(TXFIFOADD, TX_FIFO_AT / 8);
    write8(RXFIFOSZ, FIFO_512 | FIFO_DOUBLE);
    write16(RXFIFOADD, RX_FIFO_AT / 8);
    write8(INDEX, 0);
}

static const char *start(void) {
    if (!gta7lite_usb_supply()) return "VUSB33 did not turn on";
    /* Changed only where they differ, as Linux's mux and gate operations do;
     * the update bit latches the mux. Then mt_usb_enable's 10 ms. */
    uint32_t top = mmio_read32(TOPCKGEN + CLK_CFG_5);
    if (!(top & USB_TOP_UNIVPLL3_D4)) {
        mmio_write32(TOPCKGEN + CLK_CFG_5_SET, USB_TOP_UNIVPLL3_D4);
        mmio_write32(TOPCKGEN + CLK_CFG_UPDATE, USB_TOP_UPDATE);
    }
    if (top & USB_TOP_OFF) {
        mmio_write32(TOPCKGEN + CLK_CFG_5_CLR, USB_TOP_OFF);
        mmio_write32(TOPCKGEN + CLK_CFG_UPDATE, USB_TOP_UPDATE);
    }
    if (mmio_read32(INFRACFG + CG0_STA) & ICUSB) mmio_write32(INFRACFG + CG0_CLR, ICUSB);
    delay_us(10000);
    if ((mmio_read32(TOPCKGEN + CLK_CFG_5) & (USB_TOP_OFF | USB_TOP_UNIVPLL3_D4)) !=
            USB_TOP_UNIVPLL3_D4 ||
        (mmio_read32(INFRACFG + CG0_STA) & ICUSB))
        return "its clocks did not start";
    /* Out of the UART, GPIO and power-saving modes, the BC1.1 charger
     * detection switch open, then a B-device that sees VBUS. */
    delay_us(50);
    phy(PHY_ACR3, 1u << 12, 0);
    phy(PHY_DTM0,
        1u << 26 | 1u << 23 | 1u << 21 | 1u << 20 | 1u << 19 | 1u << 18 | 1u << 17 | 0xfu << 10 |
            3u << 6 | 3u << 4 | 1u << 2,
        0);
    phy(PHY_DTM1, 1u << 16, 0);
    phy(PHY_ACR4, 1u << 17 | 1u << 16 | 1u << 9 | 1u << 8, 1u << 18);
    phy(PHY_ACR6, 0xffu << 24 | 1u << 23, 0x40u << 24 | 1u << 20);
    delay_us(800);
    phy(PHY_DTM1, 0x10, 0x3fu << 8 | 0x2f); /* VBUSVALID, AVALID, BVALID, IDDIG, IDPULLUP */
    phy(PHY_ACR6, 0, 0xf0);                 /* the disconnect threshold at its highest */
    write16(SWRST, read16(SWRST) | SWRST_DISUSBRESET | SWRST_SWRST);
    delay_us(100);
    if (!read8(EPINFO)) return "its controller did not answer";
    mmio_write32(MUSB + L1INTM, 0); /* polled: nothing reaches the interrupt controller */
    write16(INTRTXE, 0);
    write16(INTRRXE, 0);
    write8(INTRUSBE, 0);
    write16(INTRTX, 0xffff);
    write16(INTRRX, 0xffff);
    write8(INTRUSB, 0xff);
    fifo_sizes();
    write16(INTRTXE, 1 | 1 << BULK);
    write16(INTRRXE, 1 << BULK);
    write8(INTRUSBE, INTR_RESET | INTR_RESUME | INTR_SUSPEND);
    write8(POWER, POWER_HSENAB);
    return 0;
}

const char *gta7lite_usb_init(void) {
    if (usb.started || usb.error) return usb.error;
    usb.packet = 512;
    usb.error = start();
    usb.started = !usb.error;
    return usb.error;
}

void gta7lite_usb_connect(void) {
    if (!usb.started || usb.connected) return;
    usb.connected = true;
    write8(POWER, POWER_HSENAB | POWER_SOFTCONN);
}

static bool high_speed(void) { return read8(POWER) & POWER_HSMODE; }

/* The bulk endpoints, configured or not; their FIFOs emptied and data
 * toggles reset as Linux's musb_gadget_enable does. */
static void configure(bool on) {
    usb.configured = on;
    usb.out_at = usb.out_end = 0;
    if (!on) return;
    usb.packet = high_speed() ? 512 : 64;
    fifo_sizes();
    write8(INDEX, BULK);
    write16(TXMAXP, usb.packet);
    uint16_t csr = TX_MODE | TX_CLRDATATOG;
    if (read16(TXCSR) & TX_FIFONOTEMPTY) csr |= TX_FLUSHFIFO;
    write16(TXCSR, csr);
    write16(TXCSR, csr);
    write16(RXMAXP, usb.packet);
    write16(RXCSR, RX_FLUSHFIFO | RX_CLRDATATOG);
    write16(RXCSR, RX_FLUSHFIFO | RX_CLRDATATOG);
    write8(INDEX, 0);
}

static unsigned configuration(unsigned char *out, bool high, unsigned char type) {
    static const unsigned char head[18] = {9, 2, 32, 0, 1, 1, 0, 0x80, 250,
                                           9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 0};
    unsigned packet = high ? 512 : 64;
    memcpy(out, head, sizeof(head));
    out[1] = type;
    for (unsigned i = 0; i < 2; ++i) {
        unsigned char *e = out + sizeof(head) + 7 * i;
        e[0] = 7;
        e[1] = 5;
        e[2] = i ? 0x01 : 0x81;
        e[3] = 2;
        e[4] = (unsigned char)packet;
        e[5] = (unsigned char)(packet >> 8);
        e[6] = 0;
    }
    return 32;
}

/* A descriptor in usb.control: its size, or 0 for none. */
static unsigned descriptor(uint16_t value) {
    unsigned char *out = usb.control, index = (unsigned char)value;
    switch (value >> 8) {
    case 1:
        memcpy(out, device_descriptor, sizeof(device_descriptor));
        return sizeof(device_descriptor);
    case 2:
        return configuration(out, high_speed(), 2);
    case 3:
        if (!index) {
            static const unsigned char languages[4] = {4, 3, 0x09, 0x04}; /* US English */
            memcpy(out, languages, sizeof(languages));
            return sizeof(languages);
        }
        if (index <= sizeof(strings) / sizeof(strings[0])) {
            const char *s = strings[index - 1];
            unsigned n = 0;
            for (; s[n]; ++n) {
                out[2 + 2 * n] = (unsigned char)s[n];
                out[3 + 2 * n] = 0;
            }
            out[0] = (unsigned char)(2 + 2 * n);
            out[1] = 3;
            return 2 + 2 * n;
        }
        return 0;
    case 6:
        memcpy(out, qualifier_descriptor, sizeof(qualifier_descriptor));
        return sizeof(qualifier_descriptor);
    case 7:
        return configuration(out, !high_speed(), 7);
    default:
        return 0;
    }
}

/* Endpoint 0's CSR once it no longer has `bits`, within CONTROL_US. */
static bool control_wait(uint16_t bits) {
    uint64_t start = time_us();
    for (;;) {
        write8(INDEX, 0);
        uint16_t csr = read16(CSR0);
        if (csr & CSR0_SETUPEND) return false;
        if (!(csr & bits)) return true;
        if (time_us() - start >= CONTROL_US) return false;
    }
}

/* The data stage of a request that reads `size` bytes of usb.control,
 * `asked` at most: packets of 64 bytes, a short or zero-length one ending
 * the stage early. The status stage then completes in the controller. */
static void control_in(unsigned size, unsigned asked) {
    write16(CSR0, CSR0_SVDRXPKTRDY);
    if (!control_wait(CSR0_RXPKTRDY)) return;
    if (size > asked) size = asked;
    for (unsigned at = 0;;) {
        unsigned n = size - at < EP0_PACKET ? size - at : EP0_PACKET;
        write8(INDEX, 0);
        fifo_write(0, usb.control + at, n);
        at += n;
        bool last = n < EP0_PACKET || at == asked;
        write16(CSR0, CSR0_TXPKTRDY | (last ? CSR0_DATAEND : 0));
        if (last || !control_wait(CSR0_TXPKTRDY)) return;
    }
}

/* SET_ADDRESS: acknowledged, then the address set once the status stage
 * ends, which endpoint 0's interrupt flag reports. */
static void set_address(uint8_t address) {
    write16(CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND);
    for (uint64_t start = time_us(); !(read16(INTRTX) & 1) && time_us() - start < CONTROL_US;) { }
    write16(INTRTX, 1);
    write8(FADDR, address);
}

/* Writing a CSR's write-zero-to-clear bits as ones keeps them. */
static void reset_toggle(uint16_t endpoint) {
    write8(INDEX, BULK);
    if (endpoint == 0x81) write16(TXCSR, read16(TXCSR) | TX_CLRDATATOG);
    else write16(RXCSR, read16(RXCSR) | RX_CLRDATATOG | RX_SENTSTALL | RX_OVERRUN | RX_PKTRDY);
    write8(INDEX, 0);
}

/* A SETUP packet waiting in endpoint 0's FIFO. */
static void setup(void) {
    unsigned char s[8];
    if (read8(COUNT0) != 8) {
        write16(CSR0, CSR0_SVDRXPKTRDY | CSR0_SENDSTALL);
        return;
    }
    fifo_read(0, s, 8);
    write16(INTRTX, 1); /* endpoint 0's next event is this request's */
    uint8_t type = s[0], request = s[1];
    uint16_t value = (uint16_t)(s[2] | s[3] << 8), index = (uint16_t)(s[4] | s[5] << 8);
    uint16_t length = (uint16_t)(s[6] | s[7] << 8);
    bool bulk = index == 0x81 || index == 0x01;
    if ((type & 0x80) && length) {
        unsigned size = 0;
        if (request == GET_DESCRIPTOR && type == 0x80) {
            size = descriptor(value);
        } else if (request == GET_STATUS && (type == 0x80 || (type == 0x81 && !index) ||
                                             (type == 0x82 && (bulk || !index)))) {
            usb.control[0] = usb.control[1] = 0; /* bus-powered; nothing halted */
            size = 2;
        } else if (request == GET_CONFIGURATION && type == 0x80) {
            usb.control[0] = usb.configured;
            size = 1;
        } else if (request == GET_INTERFACE && type == 0x81 && !index && usb.configured) {
            usb.control[0] = 0;
            size = 1;
        }
        if (size) control_in(size, length);
        else write16(CSR0, CSR0_SVDRXPKTRDY | CSR0_SENDSTALL);
        return;
    }
    if (length) { /* no request here sends the device data */
        write16(CSR0, CSR0_SVDRXPKTRDY | CSR0_SENDSTALL);
        return;
    }
    if (request == SET_ADDRESS && type == 0 && value < 128) {
        set_address((uint8_t)value);
        return;
    }
    bool handled = true;
    if (request == SET_CONFIGURATION && type == 0 && value <= 1) {
        configure(value == 1);
    } else if (request == SET_INTERFACE && type == 1 && !index && !value && usb.configured) {
        reset_toggle(0x81);
        reset_toggle(0x01);
    } else if (request == CLEAR_FEATURE && type == 2 && !value && bulk && usb.configured) {
        reset_toggle(index);
    } else {
        handled = false;
    }
    write16(CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND | (handled ? 0 : CSR0_SENDSTALL));
}

/* Bus resets and endpoint 0. True after a reset, which ends a transfer. */
static bool events(void) {
    bool reset = false;
    uint8_t flags = read8(INTRUSB);
    if (flags) write8(INTRUSB, flags);
    if (flags & INTR_RESET) {
        write8(FADDR, 0);
        configure(false);
        reset = true;
    }
    write8(INDEX, 0);
    uint16_t csr = read16(CSR0);
    if (csr & CSR0_SENTSTALL) write16(CSR0, csr & ~CSR0_SENTSTALL);
    if (csr & CSR0_SETUPEND) write16(CSR0, CSR0_SVDSETUPEND);
    if (read16(CSR0) & CSR0_RXPKTRDY) setup();
    return reset;
}

/* Whatever bulk IN data is still queued, dropped. */
static void abandon(void) {
    write8(INDEX, BULK);
    if (read16(TXCSR) & (TX_PKTRDY | TX_FIFONOTEMPTY)) write16(TXCSR, TX_MODE | TX_FLUSHFIFO);
    write8(INDEX, 0);
}

/* One bulk IN packet once the FIFO is free: false, with the stream
 * abandoned, when the host has not taken the previous one within PACKET_US
 * or reset or reconfigured the link meanwhile. */
static bool send(const unsigned char *data, unsigned n) {
    uint64_t start = time_us();
    for (;;) {
        if (!usb.configured) return false;
        write8(INDEX, BULK);
        if (!(read16(TXCSR) & TX_PKTRDY)) break;
        if (events() || time_us() - start >= PACKET_US) {
            abandon();
            return false;
        }
    }
    fifo_write(BULK, data, n);
    write16(TXCSR, (uint16_t)((read16(TXCSR) | TX_PKTRDY) & ~TX_UNDERRUN));
    write8(INDEX, 0);
    usb.queued_at = time_us();
    gta7lite_watchdog_feed();
    return true;
}

bool gta7lite_usb_send(const void *data, uint32_t size) {
    return size < usb.packet && send(data, size);
}

void gta7lite_usb_drain(void) {
    for (uint64_t start = time_us(); usb.configured && time_us() - start < DRAIN_US;) {
        write8(INDEX, BULK);
        bool queued = read16(TXCSR) & TX_PKTRDY;
        write8(INDEX, 0);
        if (!queued) return;
    }
}

/* The next bulk OUT packet into usb.out, within PACKET_US: false when the
 * host sends none or resets the link. */
static bool receive(void) {
    uint64_t start = time_us();
    for (;;) {
        if (!usb.configured) return false;
        write8(INDEX, BULK);
        uint16_t csr = read16(RXCSR);
        if (csr & RX_PKTRDY) {
            unsigned n = read16(RXCOUNT) & 0x1fff;
            if (n > sizeof(usb.out)) n = sizeof(usb.out);
            fifo_read(BULK, usb.out, n);
            write16(RXCSR, (uint16_t)((csr | RX_SENTSTALL | RX_OVERRUN) & ~RX_PKTRDY));
            write8(INDEX, 0);
            usb.out_at = 0;
            usb.out_end = n;
            gta7lite_watchdog_feed();
            return true;
        }
        if (events() || time_us() - start >= PACKET_US) return false;
    }
}

bool gta7lite_usb_read(void *data, uint32_t size) {
    unsigned char *p = data;
    while (size) {
        if (usb.out_at == usb.out_end && !receive()) {
            usb.out_at = usb.out_end = 0;
            return false;
        }
        unsigned n = usb.out_end - usb.out_at;
        if (n > size) n = size;
        memcpy(p, usb.out + usb.out_at, n);
        usb.out_at += n;
        p += n;
        size -= n;
    }
    return true;
}

/* A command from the bulk OUT endpoint: one packet. A response the host
 * never read is dropped first, so it cannot answer this command, though a
 * host usually reads it first: hence the STALE_US limit too. */
static void command(void) {
    write8(INDEX, BULK);
    uint16_t csr = read16(RXCSR);
    if (!(csr & RX_PKTRDY)) {
        write8(INDEX, 0);
        return;
    }
    unsigned n = read16(RXCOUNT) & 0x1fff;
    if (n > sizeof(usb.command) - 1) n = sizeof(usb.command) - 1;
    fifo_read(BULK, (unsigned char *)usb.command, n);
    write16(RXCSR, (uint16_t)((csr | RX_SENTSTALL | RX_OVERRUN) & ~RX_PKTRDY));
    write8(INDEX, 0);
    abandon();
    usb.command[n] = 0;
    gta7lite_fastboot(usb.command, n);
}

void gta7lite_usb_poll(void) {
    if (!usb.connected || usb.polling) return;
    usb.polling = true;
    (void)events();
    if (usb.configured) {
        write8(INDEX, BULK);
        bool queued = read16(TXCSR) & TX_PKTRDY;
        write8(INDEX, 0);
        if (queued && time_us() - usb.queued_at >= STALE_US) abandon();
        command();
    }
    usb.polling = false;
}
