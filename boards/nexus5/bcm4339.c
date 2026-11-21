#include "nexus5.h"
#include "kernel_io.h"
#include "platform.h"
#include "runtime_files.h"
#include "wire.h"

/* The Broadcom BCM4339 behind the SDIO bus (sdio.c), brought up and spoken
 * to as Linux's brcmfmac does (drivers/net/wireless/broadcom/brcm80211/
 * brcmfmac: bcmsdh.c, sdio.c, chip.c, firmware.c, bcdc.c, fwil.c, fweh.c),
 * polled. Function 1 reaches the chip's backplane through a 32 KiB window:
 * chipcommon's ID (a 4339 reads 0x4335 from revision 2), the cores found in
 * the enumeration ROM, the ARM CR4 held while its RAM (from 0x180000) takes
 * the firmware and, at its end, the board's NVRAM text with a length token,
 * then started from the firmware's first word written at address 0. Once
 * its high-throughput clock runs and function 2 is ready, frames move on
 * function 2 in SDPCM: a 4-byte length tag and its complement, then
 * sequence, channel (0 control, 1 events, 2 data, 3 glom), the data offset
 * and, from the chip, the window of sequence numbers it will accept. The
 * chip gloms the frames it sends (bus:txglom does not stop it): a descriptor
 * on channel 3, its channel byte's top bit set, lists the slots of the
 * superframe that comes next, each holding a frame of its own, the first
 * after the superframe's header (brcmfmac's brcmf_sdio_rxglom). Control
 * frames carry BCDC ioctls (a 16-byte header: command, length, flags with
 * the request ID and set bit, status); events and data carry a 4-byte BDC
 * header before an Ethernet frame. Interrupts are polled: the SDIO core's
 * status says when frames wait. Firmware and NVRAM are the stock system's
 * (/firmware/brcm, tools/package_nexus5.py). */

enum {
    /* Function 1 registers (brcmfmac sdio.h). */
    WATERMARK = 0x10008,
    WINDOW_LOW = 0x1000a,
    WINDOW_MID = 0x1000b,
    WINDOW_HIGH = 0x1000c,
    CLOCK_CSR = 0x1000e,
    PULLUP = 0x1000f,
    SLEEP_CSR = 0x1001f,
    FORCE_ALP = 0x01,
    FORCE_HT = 0x02,
    ALP_REQUEST = 0x08,
    HT_REQUEST = 0x10,
    HW_CLKREQ_OFF = 0x20,
    ALP_AVAILABLE = 0x40,
    HT_AVAILABLE = 0x80,
    ACCESS_4B = 0x8000,
    WINDOW = 0x8000,
    /* The backplane. */
    ENUM_BASE = 0x18000000,
    CHIPCOMMON = 0x800,
    D11 = 0x812,
    SDIO_DEVICE = 0x829,
    ARM_CR4 = 0x83e,
    PMU = 0x827,
    IOCTL = 0x408,
    RESET_CTL = 0x800,
    CR4_HALT = 0x20,
    RAM_BASE = 0x180000,
    /* The SDIO core's registers. */
    INTSTATUS = 0x20,
    HOSTINTMASK = 0x24,
    TOSB_MAILBOX = 0x40,
    TOSB_MAILBOX_DATA = 0x48,
    TOHOST_MAILBOX_DATA = 0x4c,
    FC_STATE = 0x10,
    FC_CHANGE = 0x20,
    FRAME_INDICATION = 0x40,
    HOST_INTERRUPT = 0x80,
    CHIP_ACTIVE = 0x20000000,
    HOST_INT_MASK = CHIP_ACTIVE | HOST_INTERRUPT | FRAME_INDICATION | FC_CHANGE | FC_STATE,
    SMB_INT_ACK = 0x02,
    /* SDPCM. */
    SDPCM_HEADER = 12,
    CONTROL = 0,
    EVENTS = 1,
    DATA = 2,
    GLOM = 3,
    GLOM_DESCRIPTOR = 0x80, /* in the channel byte */
    GLOM_SLOTS = 64,
    FIRST_READ = 64,
    RX_MAX = 8192 + 16 + 2048 + 512,
    BLOCK_F2 = 512,
    /* BCDC. */
    GET_VAR = 262,
    SET_VAR = 263,
    IOCTL_US = 2500000,
    POLL_FRAMES = 50,
    MAX_CORES = 16,
};

struct core {
    uint16_t id;
    uint8_t revision;
    uint32_t base, wrapper;
};

static struct core cores[MAX_CORES];
static unsigned core_count;
static uint32_t window = ~0u, sdio_core, ram_size;
static uint8_t tx_seq = 255, tx_max, flow_control;
static uint16_t request_id;
static bool up, failed, rx_pending;
static void (*deliver_event)(const uint8_t *message, unsigned length);
static void (*deliver_frame)(const uint8_t *ethernet, unsigned length);
static unsigned (*receive_room)(void);
static uint8_t rx[RX_MAX] __attribute__((aligned(4)));
static uint8_t tx[2048] __attribute__((aligned(4)));
static uint8_t reply[RX_MAX] __attribute__((aligned(4)));
/* At most 1518 bytes with the header, as brcmfmac sends them. */
static uint8_t message[1518] __attribute__((aligned(4)));
static uint8_t iovar_buffer[1502];
static unsigned reply_length;
static bool reply_ready;
/* The superframe a descriptor announced: its slots' count and lengths
 * (those that fit here), and their total. */
static uint16_t glom[GLOM_SLOTS];
static unsigned glom_slots, glom_total;

static bool f1_byte(uint32_t address, uint8_t *value, bool write) {
    return nexus5_sdio_byte(1, address, value, write);
}

static bool f1_set(uint32_t address, uint8_t value) { return f1_byte(address, &value, true); }

static bool set_window(uint32_t address) {
    uint32_t base = address & ~(WINDOW - 1);
    if (base == window) return true;
    window = ~0u;
    if (!f1_set(WINDOW_LOW, (uint8_t)(base >> 8)) || !f1_set(WINDOW_MID, (uint8_t)(base >> 16)) ||
        !f1_set(WINDOW_HIGH, (uint8_t)(base >> 24)))
        return false;
    window = base;
    return true;
}

static bool bp_read(uint32_t address, uint32_t *value) {
    uint8_t b[4];
    if (!set_window(address) ||
        !nexus5_sdio_transfer(1, (address & (WINDOW - 1)) | ACCESS_4B, b, 4, false, true))
        return false;
    *value = le32(b);
    return true;
}

static bool bp_write(uint32_t address, uint32_t value) {
    uint8_t b[4];
    put32(b, value);
    return set_window(address) &&
           nexus5_sdio_transfer(1, (address & (WINDOW - 1)) | ACCESS_4B, b, 4, true, true);
}

static uint32_t bp(uint32_t address) {
    uint32_t value = 0;
    if (!bp_read(address, &value)) failed = true;
    return value;
}

/* Bytes into the chip's memory, never across a window, 4-byte rounded. */
static bool ram_write(uint32_t address, const uint8_t *data, unsigned size) {
    static uint8_t chunk[2048] __attribute__((aligned(4)));
    while (size) {
        unsigned offset = address & (WINDOW - 1), n = WINDOW - offset;
        if (n > size) n = size;
        if (n > sizeof chunk) n = sizeof chunk;
        memcpy(chunk, data, n);
        unsigned padded = (n + 3) & ~3u;
        if (padded > n) memset(chunk + n, 0, padded - n);
        if (!set_window(address) ||
            !nexus5_sdio_transfer(1, offset | ACCESS_4B, chunk, padded, true, true))
            return false;
        address += n;
        data += n;
        size -= n;
        kernel_io_wait();
    }
    return true;
}

static const struct core *find(uint16_t id) {
    for (unsigned i = 0; i < core_count; ++i)
        if (cores[i].id == id) return &cores[i];
    return 0;
}

/* The enumeration ROM (chip.c brcmf_chip_dmp_*): each component's ID and
 * revision, then its register base and its wrapper (a slave wrapper, or a
 * master wrapper when a master port follows). */
static bool enumerate(void) {
    uint32_t rom = bp(ENUM_BASE + 0xfc), d;
    core_count = 0;
    for (unsigned guard = 0; guard < 512 && core_count < MAX_CORES; ++guard) {
        d = bp(rom);
        rom += 4;
        if (failed) return false;
        if ((d & 0xf) == 0xf) return core_count != 0;
        if ((d & 0xf) != 1) continue;
        uint16_t id = (uint16_t)(d >> 8 & 0xfff);
        uint32_t b = bp(rom);
        rom += 4;
        if ((b & 0xf) != 1) continue;
        uint8_t revision = (uint8_t)(b >> 24);
        unsigned wrappers = (b >> 14 & 0x1f) + (b >> 19 & 0x1f);
        if (!wrappers && id != PMU && id != 0x840) continue;
        uint32_t base = 0, wrapper = 0;
        /* A master port first means the wrapper sought is a master's. */
        uint32_t next = bp(rom);
        unsigned want = (next & 0xf) == 3 ? 3 : 2;
        for (unsigned g = 0; g < 64; ++g) {
            d = bp(rom);
            if (failed) return false;
            if ((d & 0xf) == 1 || (d & 0xf) == 0xf) break;
            rom += 4;
            if ((d & 7) != 5) continue; /* address descriptors only */
            if (d & 8) rom += 4;        /* an upper address word */
            unsigned type = d >> 6 & 3, size = d >> 4 & 3;
            if (size == 3) {
                uint32_t s = bp(rom);
                rom += 4;
                if (s & 8) rom += 4;
                continue;
            }
            if (size > 1) continue;
            if (type == 0 && !base) base = d & 0xfffff000;
            else if (type == want && !wrapper) wrapper = d & 0xfffff000;
        }
        if (!base) continue;
        cores[core_count++] = (struct core){id, revision, base, wrapper};
    }
    return false;
}

/* AI core control (chip.c brcmf_chip_ai_coredisable/resetcore). */
static bool core_disable(const struct core *c, uint32_t prereset, uint32_t reset) {
    if (!(bp(c->wrapper + RESET_CTL) & 1)) {
        if (!bp_write(c->wrapper + IOCTL, prereset | 3)) return false;
        (void)bp(c->wrapper + IOCTL);
        if (!bp_write(c->wrapper + RESET_CTL, 1)) return false;
        delay_us(20);
        for (uint64_t start = time_us(); bp(c->wrapper + RESET_CTL) != 1;)
            if (time_us() - start >= 300) return false;
    }
    if (!bp_write(c->wrapper + IOCTL, reset | 3)) return false;
    (void)bp(c->wrapper + IOCTL);
    return !failed;
}

static bool core_reset(const struct core *c, uint32_t prereset, uint32_t reset,
                       uint32_t postreset) {
    if (!core_disable(c, prereset, reset)) return false;
    for (unsigned tries = 0; tries < 50 && (bp(c->wrapper + RESET_CTL) & 1); ++tries) {
        if (!bp_write(c->wrapper + RESET_CTL, 0)) return false;
        delay_us(50);
    }
    if (bp(c->wrapper + RESET_CTL) & 1) return false;
    if (!bp_write(c->wrapper + IOCTL, postreset | 1)) return false;
    (void)bp(c->wrapper + IOCTL);
    return !failed;
}

/* The CR4's RAM: its banks' sizes from the core's capability and bank
 * registers (chip.c brcmf_chip_tcm_ramsize). */
static uint32_t cr4_ram(const struct core *c) {
    uint32_t caps = bp(c->base + 0x04), total = 0;
    unsigned banks = (caps & 15) + (caps >> 4 & 15);
    for (unsigned i = 0; i < banks && !failed; ++i) {
        if (!bp_write(c->base + 0x40, i)) return 0;
        uint32_t info = bp(c->base + 0x44);
        total += ((info & 0x7f) + 1) * (info & 0x200 ? 1024u : 8192u);
    }
    return failed || total > 4u << 20 ? 0 : total;
}

static bool clock_wait(uint8_t want, unsigned ms) {
    for (uint64_t start = time_us();;) {
        uint8_t v;
        if (!f1_byte(CLOCK_CSR, &v, false)) return false;
        if ((v & want) == want) return true;
        if (time_us() - start >= ms * 1000ull) return false;
        kernel_io_wait();
    }
}

/* NVRAM text as brcmfmac hands it to the firmware (firmware.c
 * brcmf_fw_nvram_strip): key=value entries, each ending in a NUL, comments
 * and invalid lines dropped; then NULs to a 4-byte multiple (at least one)
 * and a token of the length in words and its complement. */
static unsigned nvram(const uint8_t *text, unsigned size, uint8_t *out, unsigned room) {
    unsigned n = 0;
    for (unsigned at = 0; at < size;) {
        unsigned end = at;
        while (end < size && text[end] != '\n') ++end;
        unsigned line = at;
        at = end + 1;
        while (line < end && (text[line] == ' ' || text[line] == '\t' || text[line] == '\r'))
            ++line;
        if (line >= end || text[line] == '#') continue;
        unsigned stop = line;
        bool key = true, valid = true;
        while (stop < end && text[stop] != '#' && text[stop] != '\r') {
            uint8_t c = text[stop];
            if (key && c == '=') key = false;
            else if (c < 0x20 || c > 0x7e || (key && c == ' ')) valid = false;
            ++stop;
        }
        if (!valid || key) continue;
        if (n + (stop - line) + 1 + 8 > room) return 0;
        memcpy(out + n, text + line, stop - line);
        n += stop - line;
        out[n++] = 0;
    }
    unsigned padded = (n + 1 + 3) & ~3u;
    memset(out + n, 0, padded - n);
    uint32_t words = padded / 4;
    put32(out + padded, (~words << 16) | (words & 0xffff));
    return padded + 4;
}

static const char *download(const uint8_t *firmware, unsigned firmware_size,
                            const uint8_t *nvram_text, unsigned nvram_size) {
    static uint8_t blob[4096];
    const struct core *cr4 = find(ARM_CR4);
    unsigned blob_size = nvram(nvram_text, nvram_size, blob, sizeof blob);
    if (!blob_size) return "Wi-Fi calibration unreadable";
    if (!f1_set(CLOCK_CSR, ALP_REQUEST) || !clock_wait(ALP_AVAILABLE, 1000))
        return "Wi-Fi chip clock (ALP) off";
    uint32_t vector = le32(firmware);
    if (!ram_write(RAM_BASE, firmware, firmware_size)) return "Wi-Fi firmware not written";
    if (!ram_write(RAM_BASE + ram_size - blob_size, blob, blob_size))
        return "Wi-Fi calibration not written";
    /* Start (chip.c brcmf_chip_cr4_set_active): the SDIO core's status
     * cleared, the reset vector at 0, the CPU out of halt. */
    uint8_t v[4];
    put32(v, vector);
    if (!bp_write(sdio_core + INTSTATUS, 0xffffffffu) || !ram_write(0, v, 4) ||
        !core_reset(cr4, CR4_HALT, 0, 0))
        return "Wi-Fi chip CPU did not start";
    (void)f1_set(CLOCK_CSR, 0);
    return 0;
}

/* After the CPU starts (sdio.c brcmf_sdio_firmware_callback): its fast
 * clock, kept on; the host's protocol version in the mailbox; function 2
 * ready; the SDIO core's host interrupts; the F2 watermark. */
static const char *bus_up(void) {
    if (!f1_set(CLOCK_CSR, HT_REQUEST) || !clock_wait(HT_AVAILABLE | ALP_AVAILABLE, 2000))
        return "Wi-Fi chip clock (HT) off";
    uint8_t clock;
    if (!f1_byte(CLOCK_CSR, &clock, false) || !f1_set(CLOCK_CSR, (uint8_t)(clock | FORCE_HT)))
        return "Wi-Fi chip clock not held";
    if (!bp_write(sdio_core + TOSB_MAILBOX_DATA, 4u << 16)) return "Wi-Fi mailbox unwritable";
    if (!nexus5_sdio_block_size(2, BLOCK_F2) || !nexus5_sdio_function(2, true, 3000))
        return "Wi-Fi firmware did not start";
    if (!bp_write(sdio_core + HOSTINTMASK, HOST_INT_MASK) || !f1_set(WATERMARK, 8))
        return "Wi-Fi chip interrupts unset";
    up = true;
    return 0;
}

/* One frame from function 2 into rx: 64 bytes first, then the rest. Its
 * length, 0 when none waits, or -1 on an error. */
static int read_frame(void) {
    if (!set_window(ENUM_BASE) || !nexus5_sdio_transfer(2, 0x8000, rx, FIRST_READ, false, false))
        return -1;
    unsigned length = rx[0] | rx[1] << 8, check = rx[2] | rx[3] << 8;
    if (!length && !check) return 0;
    if ((length ^ check) != 0xffff || length < SDPCM_HEADER || length > sizeof rx) return -1;
    if (length > FIRST_READ) {
        unsigned rest = (length - FIRST_READ + 3) & ~3u;
        if (FIRST_READ + rest > sizeof rx ||
            !nexus5_sdio_transfer(2, 0x8000, rx + FIRST_READ, rest, false, false))
            return -1;
    }
    return (int)length;
}

/* The chip's flow control and window, from a frame's header. */
static void credit_from(const uint8_t *frame) {
    uint8_t window_end = frame[9];
    flow_control = frame[8];
    tx_max = (uint8_t)(window_end - tx_seq) > 0x40 ? (uint8_t)(tx_seq + 2) : window_end;
}

/* A frame by its SDPCM header: credit and flow control, then by channel. */
static void dispatch(const uint8_t *frame, unsigned length) {
    unsigned channel = frame[5] & 15, offset = frame[7];
    credit_from(frame);
    if (offset < SDPCM_HEADER || offset > length) return;
    const uint8_t *payload = frame + offset;
    unsigned n = length - offset;
    if (channel == GLOM && (frame[5] & GLOM_DESCRIPTOR)) {
        glom_slots = glom_total = 0;
        for (unsigned at = 0; at + 2 <= n; at += 2, ++glom_slots) {
            if (glom_slots < GLOM_SLOTS) glom[glom_slots] = le16(payload + at);
            glom_total += le16(payload + at);
        }
    } else if (channel == CONTROL) {
        if (n > sizeof reply) return;
        memcpy(reply, payload, n);
        reply_length = n;
        reply_ready = true;
    } else if ((channel == EVENTS || channel == DATA) && n > 4 && (payload[0] >> 4) == 2) {
        unsigned skip = 4 + 4u * payload[3];
        if (skip >= n) return;
        if (channel == EVENTS) {
            if (deliver_event) deliver_event(payload + skip, n - skip);
        } else if (deliver_frame) deliver_frame(payload + skip, n - skip);
    }
}

/* The announced superframe, read a slot at a time into rx and each slot's
 * frame dispatched; one with more slots than fit here, or a slot larger
 * than rx, is read whole and dropped. Its frames' count (1 when dropped),
 * or -1 on an error. */
static int read_superframe(void) {
    unsigned slots = glom_slots, total = glom_total;
    glom_slots = glom_total = 0;
    bool keep = slots <= GLOM_SLOTS;
    for (unsigned i = 0; keep && i < slots; ++i)
        keep = glom[i] >= SDPCM_HEADER && glom[i] <= sizeof rx;
    if (!set_window(ENUM_BASE)) return -1;
    if (!keep) {
        for (unsigned left = total, n; left; left -= n) {
            n = left < sizeof rx ? left : (unsigned)sizeof rx;
            if (!nexus5_sdio_transfer(2, 0x8000, rx, n, false, false)) return -1;
        }
        return 1;
    }
    for (unsigned i = 0; i < slots; ++i) {
        unsigned size = glom[i];
        if (!nexus5_sdio_transfer(2, 0x8000, rx, size, false, false)) return -1;
        const uint8_t *frame = rx;
        if (!i) {
            /* The superframe's own header: the whole length, on channel 3. */
            if (le16(rx) != total || (le16(rx) ^ le16(rx + 2)) != 0xffff || (rx[5] & 15) != GLOM ||
                rx[7] < SDPCM_HEADER || rx[7] > size)
                return -1;
            credit_from(rx);
            frame += rx[7];
            size -= rx[7];
        }
        unsigned length = le16(frame);
        if ((length ^ le16(frame + 2)) != 0xffff || length < SDPCM_HEADER || length > size)
            return -1;
        dispatch(frame, length);
    }
    return (int)slots;
}

/* The SDIO core's interrupt status: acknowledged, the mailbox answered,
 * and frames read while they wait, as brcmfmac reads them (rxpending): a
 * poll reads no more than the radio has room for and leaves the rest in the
 * chip for the next, which reads on without a new indication; a wait for a
 * reply or for credit reads regardless, since its frame may come behind
 * them. False when the chip stopped answering. */
static bool service(bool waiting) {
    uint32_t status;
    if (!bp_read(sdio_core + INTSTATUS, &status)) return false;
    status &= HOST_INT_MASK;
    if (status && !bp_write(sdio_core + INTSTATUS, status)) return false;
    if (status & HOST_INTERRUPT) {
        uint32_t mail;
        if (!bp_read(sdio_core + TOHOST_MAILBOX_DATA, &mail) ||
            !bp_write(sdio_core + TOSB_MAILBOX, SMB_INT_ACK))
            return false;
        if (mail & 0x10) return false; /* the firmware halted */
        if (mail & 0x01) status |= FRAME_INDICATION;
    }
    if (status & FRAME_INDICATION) rx_pending = true;
    for (unsigned frames = 0; rx_pending && frames < POLL_FRAMES && (waiting || receive_room());
         ++frames) {
        int length;
        if (glom_total) length = read_superframe();
        else if ((length = read_frame()) > 0) dispatch(rx, (unsigned)length);
        if (length < 0) return false;
        if (!length) rx_pending = false;
    }
    return true;
}

/* Whether the chip's window takes another frame. */
static bool credit(void) {
    uint8_t room = (uint8_t)(tx_max - tx_seq);
    return room && room < 0x80;
}

/* A frame on a channel: its SDPCM header, then the payload, padded to 4
 * bytes and, past a block, to whole blocks. Without credit, a control frame
 * waits for the chip to grant it, on the service worker, which yields
 * meanwhile; a data frame, mostly from the main loop, where nothing yields,
 * reads the chip's frames once and is refused if still without credit: the
 * station or TCP sends it again. */
static bool send_frame(unsigned channel, const uint8_t *payload, unsigned n) {
    unsigned length = SDPCM_HEADER + n;
    if (length > sizeof tx) return false;
    for (uint64_t start = time_us(); !credit();) {
        if (!service(channel == CONTROL)) return false;
        if (credit()) break;
        if (channel != CONTROL || time_us() - start >= IOCTL_US) return false;
        kernel_io_wait();
    }
    memset(tx, 0, SDPCM_HEADER);
    tx[0] = (uint8_t)length;
    tx[1] = (uint8_t)(length >> 8);
    tx[2] = (uint8_t)~length;
    tx[3] = (uint8_t)(~length >> 8);
    tx[4] = tx_seq;
    tx[5] = (uint8_t)channel;
    tx[7] = SDPCM_HEADER;
    memcpy(tx + SDPCM_HEADER, payload, n);
    unsigned padded = (length + 3) & ~3u;
    if (padded > BLOCK_F2) padded = (padded + BLOCK_F2 - 1) & ~(BLOCK_F2 - 1u);
    if (padded > sizeof tx) padded = (length + 3) & ~3u;
    memset(tx + length, 0, padded - length);
    if (!set_window(ENUM_BASE) || !nexus5_sdio_transfer(2, 0x8000, tx, padded, true, true))
        return false;
    ++tx_seq;
    return true;
}

/* A BCDC ioctl, waited for: its reply's payload (up to out_length bytes)
 * in buffer. 0, a negative firmware error, or -1000 when unanswered. */
int nexus5_bcm_ioctl(unsigned command, bool set, void *buffer, unsigned length,
                     unsigned out_length) {
    if (!up || length > sizeof message - 16) return -1000;
    uint16_t id = ++request_id;
    put32(message, command);
    put32(message + 4, length > out_length ? length : out_length);
    put32(message + 8, (uint32_t)id << 16 | (set ? 2u : 0));
    put32(message + 12, 0);
    memcpy(message + 16, buffer, length);
    reply_ready = false;
    if (!send_frame(CONTROL, message, 16 + length)) return -1000;
    for (uint64_t start = time_us(); time_us() - start < IOCTL_US;) {
        if (!service(true)) return -1000;
        if (reply_ready) {
            reply_ready = false;
            if (reply_length < 16 || (le32(reply + 8) >> 16) != id) continue;
            if (le32(reply + 8) & 1) return (int)le32(reply + 12);
            unsigned n = reply_length - 16;
            if (!set && out_length) memcpy(buffer, reply + 16, n < out_length ? n : out_length);
            return 0;
        }
        kernel_io_wait();
    }
    return -1000;
}

/* An iovar: its name, a NUL, then the value (sent, or room for the
 * answer, which comes back from offset 0). */
int nexus5_bcm_iovar(const char *name, bool set, void *value, unsigned length) {
    unsigned n = (unsigned)strlen(name) + 1;
    if (n + length > sizeof iovar_buffer) return -1000;
    memcpy(iovar_buffer, name, n);
    if (set) memcpy(iovar_buffer + n, value, length);
    else memset(iovar_buffer + n, 0, length);
    int result = nexus5_bcm_ioctl(set ? SET_VAR : GET_VAR, set, iovar_buffer, n + length,
                                  set ? 0 : n + length);
    if (!result && !set) memcpy(value, iovar_buffer, length);
    return result;
}

int nexus5_bcm_iovar_int(const char *name, uint32_t value) {
    uint8_t v[4];
    put32(v, value);
    return nexus5_bcm_iovar(name, true, v, 4);
}

bool nexus5_bcm_send(const uint8_t *ethernet, unsigned length, unsigned priority) {
    static uint8_t frame[4 + 1514];
    if (!up || length < 14 || length > 1514 || (flow_control >> (priority & 7) & 1)) return false;
    frame[0] = 0x20;
    frame[1] = (uint8_t)(priority & 7);
    frame[2] = 0;
    frame[3] = 0;
    memcpy(frame + 4, ethernet, length);
    return send_frame(DATA, frame, 4 + length);
}

bool nexus5_bcm_poll(void) { return up && service(false); }

static const struct runtime_file *firmware_file(void) {
    const struct runtime_file *fw = runtime_file_find("/firmware/brcm/fw_bcmdhd.bin");
    return fw && fw->size >= 1024 ? fw : 0;
}

static const struct runtime_file *calibration_file(void) {
    return runtime_file_find("/firmware/brcm/bcmdhd.cal");
}

bool nexus5_bcm_firmware(void) { return firmware_file() && calibration_file(); }

void nexus5_bcm_stop(void) {
    up = false;
    failed = true;
    rx_pending = reply_ready = false;
    glom_slots = glom_total = 0;
    memset(rx, 0, sizeof rx);
    memset(tx, 0, sizeof tx);
    memset(reply, 0, sizeof reply);
    memset(message, 0, sizeof message);
    memset(iovar_buffer, 0, sizeof iovar_buffer);
}

const char *nexus5_bcm_start(void (*event)(const uint8_t *, unsigned),
                             void (*frame)(const uint8_t *, unsigned), unsigned (*room)(void)) {
    deliver_event = event;
    deliver_frame = frame;
    receive_room = room;
    if (up) return 0;
    if (failed) return "Wi-Fi chip failed";
    const struct runtime_file *fw = firmware_file(), *cal = calibration_file();
    if (!fw || !cal) return "Wi-Fi firmware missing";
    window = ~0u;
    tx_seq = 255;
    tx_max = 0;
    rx_pending = false;
    glom_slots = glom_total = 0;
    if (!nexus5_sdio_block_size(1, 64) || !nexus5_sdio_function(1, true, 1000))
        return "Wi-Fi bus function 1 unavailable";
    /* The backplane's always-on clock, requested and then forced
     * (brcmf_sdio_buscoreprep), before anything on the backplane. */
    if (!f1_set(CLOCK_CSR, HW_CLKREQ_OFF | ALP_REQUEST) || !clock_wait(ALP_AVAILABLE, 1000) ||
        !f1_set(CLOCK_CSR, HW_CLKREQ_OFF | FORCE_ALP))
        return "Wi-Fi chip clock (ALP) off";
    delay_us(65);
    (void)f1_set(PULLUP, 0);
    uint32_t id = bp(ENUM_BASE);
    uint16_t chip_id = (uint16_t)id;
    uint8_t chip_revision = (uint8_t)(id >> 16 & 15);
    if (chip_id == 0x4335 && chip_revision >= 2) chip_id = 0x4339;
    if (failed || chip_id != 0x4339) return "Wi-Fi chip is not a BCM4339";
    if (!enumerate()) return "Wi-Fi chip cores unreadable";
    const struct core *cr4 = find(ARM_CR4), *sdio = find(SDIO_DEVICE), *cc = find(CHIPCOMMON);
    if (!cr4 || !sdio || !cc) return "Wi-Fi chip cores missing";
    sdio_core = sdio->base;
    /* Keep the SDIO core awake (rev 12 and later) and let a card reset
     * reset the backplane too. */
    if (sdio->revision >= 12) {
        uint8_t sleep;
        if (f1_byte(SLEEP_CSR, &sleep, false) && !(sleep & 1)) (void)f1_set(SLEEP_CSR, sleep | 1);
    }
    uint8_t control;
    if (nexus5_sdio_byte(0, 0xf1, &control, false)) {
        control |= 2;
        (void)nexus5_sdio_byte(0, 0xf1, &control, true);
    }
    /* The CPU halted; the 802.11 cores in reset. */
    uint32_t halted = bp(cr4->wrapper + IOCTL) & CR4_HALT;
    if (!core_reset(cr4, halted, CR4_HALT, CR4_HALT)) return "Wi-Fi chip CPU not halted";
    for (unsigned i = 0; i < core_count; ++i)
        if (cores[i].id == D11 && !core_disable(&cores[i], 0x0c, 0x04))
            return "Wi-Fi radio core not stopped";
    ram_size = cr4_ram(cr4);
    if (!ram_size) return "Wi-Fi chip memory unknown";
    (void)nexus5_sdio_function(2, false, 0);
    const char *why = download(runtime_file_data(fw), fw->size, runtime_file_data(cal), cal->size);
    if (why) return why;
    why = bus_up();
    return why;
}
