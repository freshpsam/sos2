#include "pc.h"
#include "net_device.h"
#include "platform.h"

/* An Intel 82574L Ethernet controller (e1000e), QEMU's PC network card, as
 * the network stack's device (include/net_device.h), polled: reset, its MAC
 * address from the receive address registers the hardware loads from its
 * EEPROM (else the EEPROM itself), legacy descriptor rings with 2 KiB
 * buffers in BSS, the CRC stripped from received frames, transmit copied
 * in. The PHY's autonegotiation is the hardware's own. Interrupts stay
 * masked.
 * https://www.intel.com/content/www/us/en/content-details/333016 (82574
 * datasheet: 10, the registers; 7.1 and 7.2, receive and transmit) */
enum {
    CTRL = 0x0000,
    STATUS = 0x0008,
    EERD = 0x0014,
    ICR = 0x00c0,
    IMC = 0x00d8,
    RCTL = 0x0100,
    TCTL = 0x0400,
    TIPG = 0x0410,
    RDBAL = 0x2800,
    RDBAH = 0x2804,
    RDLEN = 0x2808,
    RDH = 0x2810,
    RDT = 0x2818,
    TDBAL = 0x3800,
    TDBAH = 0x3804,
    TDLEN = 0x3808,
    TDH = 0x3810,
    TDT = 0x3818,
    MTA = 0x5200,
    RAL = 0x5400,
    RAH = 0x5404,
    CTRL_ASDE = 1 << 5,
    CTRL_SLU = 1 << 6,
    CTRL_RST = 1 << 26,
    STATUS_LU = 1 << 1,
    RCTL_EN = 1 << 1,
    RCTL_BAM = 1 << 15,
    RCTL_SECRC = 1 << 26,
    TCTL_EN = 1 << 1,
    TCTL_PSP = 1 << 3,
    TCTL_RTLC = 1 << 24,
    DONE = 1 << 0,
    EOP = 1 << 1,
    TX_EOP = 1 << 0,
    TX_IFCS = 1 << 1,
    TX_RS = 1 << 3,
};

#define RING   64
#define BUFFER 2048

struct rx_descriptor {
    uint64_t buffer;
    uint16_t length, checksum;
    uint8_t status, errors;
    uint16_t special;
};

struct tx_descriptor {
    uint64_t buffer;
    uint16_t length;
    uint8_t checksum_offset, command, status, checksum_start;
    uint16_t special;
};

static struct rx_descriptor rx[RING] __attribute__((aligned(128)));
static struct tx_descriptor tx[RING] __attribute__((aligned(128)));
static uint8_t rx_buffers[RING][BUFFER] __attribute__((aligned(BUFFER)));
static uint8_t tx_buffers[RING][BUFFER] __attribute__((aligned(BUFFER)));
static const struct pci_function *card;
static struct net_device *net;
static uintptr_t registers;
static unsigned rx_next, tx_tail, tx_clean;
static bool running;

static uint32_t read(unsigned offset) { return mmio_read32(registers + offset); }

static void write(unsigned offset, uint32_t value) { mmio_write32(registers + offset, value); }

void e1000e_probe(struct platform_devices *d) {
    card = 0;
    for (unsigned i = 0; !card; ++i) {
        const struct pci_function *f = pci_find(0x020000, 0xffff00, i);
        if (!f) return;
        if (f->vendor == 0x8086 && f->id == 0x10d3 && pci_map(d, f, 0)) card = f;
    }
}

static bool online(void) { return running; }

static bool link(void) { return running && (read(STATUS) & STATUS_LU); }

static void poll(void (*receive)(const void *, unsigned)) {
    if (!running) return;
    for (unsigned budget = 0; budget < 32; ++budget) {
        struct rx_descriptor *d = &rx[rx_next];
        if (!(d->status & DONE)) break;
        barrier();
        if ((d->status & EOP) && !d->errors && d->length >= 14 && d->length <= NET_FRAME_MAX) {
            receive(rx_buffers[rx_next], d->length);
            ++net->rx_packets;
        }
        d->status = 0;
        barrier();
        write(RDT, rx_next);
        rx_next = (rx_next + 1) % RING;
    }
}

static bool send(const void *frame, unsigned length) {
    if (!running || !length || length > NET_FRAME_MAX) return false;
    while (tx_clean != tx_tail && (tx[tx_clean].status & DONE)) tx_clean = (tx_clean + 1) % RING;
    unsigned next = (tx_tail + 1) % RING;
    if (next == tx_clean) return false;
    memcpy(tx_buffers[tx_tail], frame, length);
    tx[tx_tail] = (struct tx_descriptor){
        (uintptr_t)tx_buffers[tx_tail], (uint16_t)length, 0, TX_EOP | TX_IFCS | TX_RS, 0, 0, 0};
    barrier();
    tx_tail = next;
    write(TDT, tx_tail);
    ++net->tx_packets;
    return true;
}

static void stop(void) {
    if (!running) return;
    write(RCTL, 0);
    write(TCTL, 0);
    running = false;
}

/* The MAC address: the first receive address, or the EEPROM's first three
 * words (EERD: the word's address from bit 2, done at bit 1). */
static bool mac_address(uint8_t mac[6]) {
    uint32_t low = read(RAL), high = read(RAH);
    if (high >> 31) {
        for (unsigned i = 0; i < 4; ++i) mac[i] = (uint8_t)(low >> (8 * i));
        mac[4] = (uint8_t)high;
        mac[5] = (uint8_t)(high >> 8);
    } else {
        for (unsigned word = 0; word < 3; ++word) {
            write(EERD, word << 2 | 1);
            uint32_t value;
            for (uint64_t deadline = time_us() + 10000; !((value = read(EERD)) & 2);)
                if (time_us() > deadline) return false;
            mac[2 * word] = (uint8_t)(value >> 16);
            mac[2 * word + 1] = (uint8_t)(value >> 24);
        }
    }
    bool zero = true;
    for (unsigned i = 0; i < 6; ++i) zero &= !mac[i];
    return !zero && !(mac[0] & 1);
}

bool e1000e_init(const struct platform_devices *devices, struct net_device *device) {
    (void)devices;
    net = device;
    *device = (struct net_device){online, link, poll, send, stop, {0}, 0, 0};
    running = false;
    if (!card) return false;
    pci_enable(card);
    registers = (uintptr_t)card->bar[0];
    write(IMC, ~0u);
    write(CTRL, read(CTRL) | CTRL_RST);
    delay_us(1000);
    for (uint64_t deadline = time_us() + 1000000; read(CTRL) & CTRL_RST;)
        if (time_us() > deadline) return false;
    write(IMC, ~0u);
    (void)read(ICR);
    write(CTRL, read(CTRL) | CTRL_SLU | CTRL_ASDE);
    if (!mac_address(device->mac)) return false;
    write(RAL, (uint32_t)device->mac[0] | (uint32_t)device->mac[1] << 8 |
                   (uint32_t)device->mac[2] << 16 | (uint32_t)device->mac[3] << 24);
    write(RAH, (uint32_t)device->mac[4] | (uint32_t)device->mac[5] << 8 | 1u << 31);
    for (unsigned i = 0; i < 128; ++i) write(MTA + 4 * i, 0);
    for (unsigned i = 0; i < RING; ++i) {
        rx[i] = (struct rx_descriptor){.buffer = (uintptr_t)rx_buffers[i]};
        tx[i] = (struct tx_descriptor){0};
    }
    rx_next = tx_tail = tx_clean = 0;
    write(RDBAL, (uint32_t)(uintptr_t)rx);
    write(RDBAH, (uint32_t)((uint64_t)(uintptr_t)rx >> 32));
    write(RDLEN, sizeof(rx));
    write(RDH, 0);
    write(RDT, RING - 1);
    write(RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    write(TDBAL, (uint32_t)(uintptr_t)tx);
    write(TDBAH, (uint32_t)((uint64_t)(uintptr_t)tx >> 32));
    write(TDLEN, sizeof(tx));
    write(TDH, 0);
    write(TDT, 0);
    write(TIPG, 0x0060200a);
    write(TCTL, TCTL_EN | TCTL_PSP | 15u << 4 | 63u << 12 | TCTL_RTLC);
    running = true;
    return true;
}
