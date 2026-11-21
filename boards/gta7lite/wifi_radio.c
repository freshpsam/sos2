/* MediaTek's Wi-Fi firmware on the Galaxy Tab A7 Lite (CONNAC1, "soc1_0",
 * with the MT6631 radio chip) as the shipped gen4m driver runs it over AXI
 * (wlan_drv_gen4m.ko; connac.c, cmm_asic_connac.c, fw_dl.c, hal_pdma.c,
 * nic_tx.c, nic_rx.c, cnm*.c, saa_fsm.c, ais_fsm.c): driver own, the DMA
 * rings (TX 0 data, TX 15 commands and management frames, TX 3 firmware
 * download; RX 0 data, RX 1 events) in the pool connsys.c reserves, the
 * firmware download, and then the commands a station needs: scans, the
 * channel, BSS and station records around a join, keys. Data leaves as
 * Ethernet frames that the hardware turns into 802.11 and arrives the same
 * way, so frames are converted for the station, which builds and parses
 * 802.11 itself. Everything is polled. */
#include "wifi_radio.h"
#include "gta7lite.h"
#include "platform.h"
#include "wire.h"
#include "wifi_device.h"
#include "runtime_files.h"
#include <string.h>

enum {
    WF = 0x18000000u,
    LPCTL = WF + 0x7000,
    HIF_ON_IRQ_ENA = WF + 0x7008,
    DUMMY_CR = WF + 0x40a8,
    HIF_RST = WF + 0x4100,
    SLP_PROT = WF + 0x4154,
    APSRC_ACK = WF + 0x4160,
    SW_INT_ENA = WF + 0x41f4,
    INT_MSK = WF + 0x4204,
    GLO_CFG = WF + 0x4208,
    RST_PTR = WF + 0x420c,
    PAUSE_TX_Q = WF + 0x4224,
    PAUSE_RX_TH10 = WF + 0x4260,
    PAUSE_RX_TH32 = WF + 0x4264,
    TX_RING = WF + 0x4300, /* per ring +0x10: BASE, CNT, CIDX, DIDX */
    RX_RING = WF + 0x4400,
    TX_EXT = WF + 0x4500,
    RX_EXT = WF + 0x4580,
    DMASHDL = WF + 0x6000,
    CONN_ON_MISC = WF + 0xc1140, /* firmware ready: bits 2:1 */
    POOL = GTA7LITE_WIFI_POOL,
    TX0_DESC = POOL,
    TX15_DESC = POOL + 0x1000,
    TX3_DESC = POOL + 0x2000,
    RX0_DESC = POOL + 0x3000,
    RX1_DESC = POOL + 0x4000,
    CMD_BUF = POOL + 0x5000, /* 256 x 4 KiB: cell i of TX 3 and of TX 15 */
    CMD_BUF_SIZE = 0x1000,
    RX0_BUF = POOL + 0x105000,
    RX0_BUF_SIZE = 0x930,
    RX1_BUF = POOL + 0x198000,
    RX1_BUF_SIZE = 0xf00,
    TOKEN_BUF = POOL + 0x1a7000, /* TXD, TXP, then the Ethernet frame */
    TOKEN_BUF_SIZE = 0x640,
    TOKENS = 128,
    TX_CELLS = 256,
    RX0_CELLS = 256,
    RX1_CELLS = 16,
    FW_CHUNK = 2048,
    /* Indexes for the station: BSS 0 with own-MAC 1, the AP's STA record 0
     * at WTBL entry 5, broadcast entry 4, entry 31 without a STA record. */
    BMC_WLAN = 4,
    AP_WLAN = 5,
    NO_STA_WLAN = 31,
    /* Firmware commands and events (wsys_cmd_handler_fw.h) */
    CMD_BASIC_CONFIG = 0x02,
    CMD_SCAN = 0x03,
    CMD_KEY = 0x07,
    CMD_DOMAIN = 0x0f,
    CMD_BSS_ACTIVATE = 0x11,
    CMD_BSS_INFO = 0x12,
    CMD_STA_RECORD = 0x13,
    CMD_STA_REMOVE = 0x14,
    CMD_PM_ABORT = 0x17,
    CMD_SCAN_CANCEL = 0x1b,
    CMD_CHANNEL = 0x1c,
    CMD_CAPABILITY = 0x8a,
    EVENT_SCAN_DONE = 0x0d,
    EVENT_CHANNEL = 0x10,
    EVENT_BEACON_TIMEOUT = 0x13,
    EVENT_PKEY_DONE = 0x24,
    EVENT_CAPABILITY = 0xec,
};

static uint8_t sequence, scan_sequence, channel_token, next_pid = 1;
static bool started, asleep, channel_held, pkey_done, beacon_lost, sta_record_live;
static unsigned scan_running; /* the scan sequence in progress, 0 without */
static uint8_t scan_done_sequence;
static uint8_t grant[24];
static bool granted;
static bool token_used[TOKENS];
static unsigned tokens_out;
/* The firmware's answer to a command awaiting one: its sequence and event. */
static uint8_t awaited, answered_eid;
static bool answered;
static uint8_t packet[64 + FW_CHUNK], rx_buffer[2400], rebuilt[2400];

/* The station's step in progress (configure is called until it answers). */
static struct radio_job {
    enum wifi_action action;
    bool active;
    unsigned step;
    uint64_t deadline;
} job;

static uint32_t rd(uintptr_t a) { return mmio_read32(a); }

static void wr(uintptr_t a, uint32_t v) { mmio_write32(a, v); }

static void dma_barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static uintptr_t ring(uintptr_t base, unsigned n, unsigned field) {
    return base + 0x10 * n + 4 * field;
}

/* Bytes into and out of the pool (Device memory) as aligned words. */
static void copy_out(uintptr_t to, const uint8_t *from, unsigned n) {
    for (unsigned i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (unsigned b = 0; b < 4 && i + b < n; ++b) w |= (uint32_t)from[i + b] << 8 * b;
        wr(to + i, w);
    }
}

static void copy_in(uint8_t *to, uintptr_t from, unsigned n) {
    for (unsigned i = 0; i < n; i += 4) {
        uint32_t w = rd(from + i);
        for (unsigned b = 0; b < 4 && i + b < n; ++b) to[i + b] = (uint8_t)(w >> 8 * b);
    }
}

static bool wait_bits(uintptr_t address, uint32_t mask, uint32_t value, unsigned timeout_us,
                      unsigned step_us) {
    uint64_t start = time_us();
    while ((rd(address) & mask) != value) {
        if (time_us() - start > timeout_us) return false;
        if (step_us) delay_us(step_us);
    }
    return true;
}

/* A packet onto a command ring (HW TX 3 or 15): the next cell's buffer, its
 * descriptor (address, length rounded to words, last segment), then the CPU
 * index as the doorbell (kalDevPortWrite). */
static bool tx_command(unsigned hw_ring, uintptr_t descs, const uint8_t *p, unsigned n) {
    uint32_t idx = rd(ring(TX_RING, hw_ring, 2));
    if (idx >= TX_CELLS || n > CMD_BUF_SIZE ||
        (idx + 1) % TX_CELLS == rd(ring(TX_RING, hw_ring, 3)))
        return false;
    uintptr_t buffer = CMD_BUF + idx * CMD_BUF_SIZE, d = descs + 16 * idx;
    copy_out(buffer, p, n);
    wr(d + 0, (uint32_t)buffer);
    wr(d + 8, 0);
    wr(d + 12, 0);
    wr(d + 4, 0x40000000u | ((n + 3) & ~3u) << 16);
    dma_barrier();
    wr(ring(TX_RING, hw_ring, 2), (idx + 1) % TX_CELLS);
    return true;
}

/* One received packet from RX ring r into rx_buffer, if its next
 * descriptor is done: its length, else 0. */
static unsigned rx_take(unsigned r) {
    unsigned cells = r ? RX1_CELLS : RX0_CELLS, size = r ? RX1_BUF_SIZE : RX0_BUF_SIZE;
    uintptr_t descs = r ? RX1_DESC : RX0_DESC, buffers = r ? RX1_BUF : RX0_BUF;
    uint32_t next = (rd(ring(RX_RING, r, 2)) + 1) % cells;
    uintptr_t d = descs + 16 * next;
    uint32_t dw1 = rd(d + 4);
    if (!(dw1 & 0x80000000u)) return 0;
    unsigned length = dw1 >> 16 & 0x3fff;
    if (length > sizeof(rx_buffer)) length = sizeof(rx_buffer);
    copy_in(rx_buffer, buffers + next * size, length);
    wr(d + 0, buffers + next * size);
    wr(d + 8, 0);
    wr(d + 4, (dw1 & 0x4000ffffu) | (uint32_t)size << 16);
    dma_barrier();
    wr(ring(RX_RING, r, 2), next);
    return (dw1 & 0x40000000u) ? length : 0; /* segmented packets are dropped */
}

/* An INIT packet (firmware download): the 32-byte hardware TXD, the init
 * header, the payload. Commands: PQ 0x8000, format 2; data: PQ 0xF800,
 * format 3 and CID 0 (asicFillInitCmdTxd). */
static unsigned init_packet(uint8_t cid, const uint8_t *payload, unsigned n) {
    unsigned length = 64 + n;
    memset(packet, 0, 64);
    put16(packet, length);
    packet[3] = cid ? 0x80 : 0xf8;
    packet[5] = 0xa0;
    packet[7] = cid ? 2 : 3;
    put16(packet + 0x20, length - 32);
    packet[0x24] = cid;
    packet[0x25] = 0xa0;
    packet[0x27] = ++sequence;
    memcpy(packet + 64, payload, n);
    return length;
}

/* An INIT command on TX ring 3 and its event on RX ring 1 within 3 s:
 * event ID eid, the same sequence and, for a command result, status 0. */
static bool init_command(uint8_t cid, const uint8_t *payload, unsigned n, uint8_t eid,
                         uint8_t *event) {
    unsigned length = init_packet(cid, payload, n);
    uint8_t seq = packet[0x27];
    if (!tx_command(3, TX3_DESC, packet, length)) return false;
    uint64_t start = time_us();
    for (;;) {
        unsigned got = rx_take(1);
        if (got >= 0x1c && rx_buffer[0x14] == eid && rx_buffer[0x15] == seq) {
            if (event) memcpy(event, rx_buffer, got < 64 ? got : 64);
            return eid != 1 || !rx_buffer[0x18];
        }
        if (time_us() - start > 3000000) return false;
    }
}

/* Driver own, DMASHDL, the rings, PDMA on (asicPcieDmaShdlInit,
 * halWpdmaInitRing, asicPdmaConfig). */
static const char *hif_init(void) {
    wr(HIF_ON_IRQ_ENA, 1);
    wr(LPCTL, 2);
    if (!wait_bits(LPCTL, 1, 0, 2048000, 1000)) return "Wi-Fi did not hand over control";
    if (rd(DUMMY_CR) == 0x0f) wr(DUMMY_CR, 0x13);
    wr(DMASHDL + 0x1c, (rd(DMASHDL + 0x1c) & 0xf000f000u) | 0x00180001u);
    wr(DMASHDL + 0x10, 0xfffe0000u);
    (void)rd(DMASHDL + 0x100);
    wr(DMASHDL + 0x20, 0x0fff0003u);
    wr(DMASHDL + 0x24, 0);
    wr(DMASHDL + 0x28, 0);
    wr(DMASHDL + 0xd0, 0);
    for (uintptr_t a = DMASHDL + 0x2c; a <= DMASHDL + 0x58; a += 4) wr(a, 0);
    for (unsigned i = 0; i < TX_CELLS; ++i)
        for (uintptr_t base = TX0_DESC; base <= TX3_DESC; base += 0x1000) {
            wr(base + 16 * i + 0, 0);
            wr(base + 16 * i + 4, 0x80000000u);
            wr(base + 16 * i + 8, 0);
            wr(base + 16 * i + 12, 0);
        }
    for (unsigned i = 0; i < RX0_CELLS; ++i) {
        wr(RX0_DESC + 16 * i + 0, RX0_BUF + i * RX0_BUF_SIZE);
        wr(RX0_DESC + 16 * i + 4, (uint32_t)RX0_BUF_SIZE << 16);
        wr(RX0_DESC + 16 * i + 8, 0);
        wr(RX0_DESC + 16 * i + 12, 0);
    }
    for (unsigned i = 0; i < RX1_CELLS; ++i) {
        wr(RX1_DESC + 16 * i + 0, RX1_BUF + i * RX1_BUF_SIZE);
        wr(RX1_DESC + 16 * i + 4, (uint32_t)RX1_BUF_SIZE << 16);
        wr(RX1_DESC + 16 * i + 8, 0);
        wr(RX1_DESC + 16 * i + 12, 0);
    }
    dma_barrier();
    wr(INT_MSK, rd(INT_MSK) & 0xdf00000cu);
    wr(GLO_CFG, rd(GLO_CFG) & ~0x5u);
    wr(PAUSE_TX_Q, 0);
    wr(SW_INT_ENA, 0x3c);
    wr(APSRC_ACK, rd(APSRC_ACK) | 0x10);
    (void)wait_bits(GLO_CFG, 0xa, 0, 101000, 1000);
    wr(RST_PTR, 0xffffffffu);
    wr(SLP_PROT, rd(SLP_PROT) | 1);
    if (!wait_bits(SLP_PROT, 1u << 16, 1u << 16, 20000, 1)) return "Wi-Fi bus did not pause";
    wr(HIF_RST, 0x20);
    wr(HIF_RST, 0x30);
    wr(SLP_PROT, rd(SLP_PROT) & ~1u);

    /* TX rings 0, 15 and 3 (their extension registers by software index, as
     * gen4m writes them), RX rings 0 and 1 owning all but one descriptor. */
    static const struct {
        unsigned hw;
        uintptr_t descs, ext;
    } tx[] = {
        {0, TX0_DESC, TX_EXT + 0x0}, {15, TX15_DESC, TX_EXT + 0xc}, {3, TX3_DESC, TX_EXT + 0x10}};

    for (unsigned i = 0; i < 3; ++i) {
        wr(ring(TX_RING, tx[i].hw, 0), (uint32_t)tx[i].descs);
        wr(ring(TX_RING, tx[i].hw, 2), 0);
        wr(ring(TX_RING, tx[i].hw, 1), TX_CELLS);
        wr(tx[i].ext, 0);
    }
    wr(ring(RX_RING, 0, 0), RX0_DESC);
    wr(ring(RX_RING, 0, 2), RX0_CELLS - 1);
    wr(ring(RX_RING, 0, 1), RX0_CELLS);
    wr(RX_EXT + 0, 0);
    wr(ring(RX_RING, 1, 0), RX1_DESC);
    wr(ring(RX_RING, 1, 2), RX1_CELLS - 1);
    wr(ring(RX_RING, 1, 1), RX1_CELLS);
    wr(RX_EXT + 4, 0);
    wr(INT_MSK, (rd(INT_MSK) & 0xdf00000cu) | 0x20080093u);
    wr(GLO_CFG, (rd(GLO_CFG) & ~0x400u) | 0x44001875u);
    wr(PAUSE_TX_Q, 0);
    wr(SW_INT_ENA, 0x3c);
    wr(APSRC_ACK, rd(APSRC_ACK) | 0x10);
    wr(PAUSE_TX_Q, 0);
    wr(PAUSE_RX_TH10, 0x00020002u);
    wr(PAUSE_RX_TH32, 0x00020002u);
    wr(DUMMY_CR, 0x13);
    return 0;
}

/* The firmware (wlanDownloadFW for connac): tailers at the file's end,
 * regions back to back from offset 0; encrypted regions announced and sent
 * in 2 KiB packets on TX ring 3, EMI regions copied, then WIFI_START and
 * the ready bits. */
static const char *firmware(void) {
    const struct runtime_file *f =
        runtime_file_find("/firmware/connsys/WIFI_RAM_CODE_soc1_0_1_1.bin");
    if (!f || f->size < 36 + 40) return "Wi-Fi firmware missing";
    const uint8_t *data = runtime_file_data(f), *common = data + f->size - 36;
    unsigned regions = common[2];
    if (!regions || regions > 10 || f->size < 36 + 40 * regions) return "Wi-Fi firmware invalid";
    const uint8_t *tailers = common - 40 * regions;
    uint32_t entry = 0, offset = 0;
    for (unsigned i = 0; i < regions; ++i)
        if (tailers[40 * i + 0x18] & 0x20) entry = le32(tailers + 40 * i + 0x10);
    wr(GLO_CFG, rd(GLO_CFG) | 0x200u);
    for (unsigned i = 0; i < regions; ++i) {
        const uint8_t *t = tailers + 40 * i;
        uint32_t address = le32(t + 0x10), length = le32(t + 0x14);
        uint8_t feature = t[0x18];
        if (length > (uint32_t)(tailers - data) - offset) return "Wi-Fi firmware invalid";
        const uint8_t *payload = data + offset;
        offset += length;
        if (feature & 0x40) continue;
        if (feature & 0x80) {
            if ((address & 0xffffff) + length > GTA7LITE_CONNSYS_EMI_SIZE)
                return "Wi-Fi firmware invalid";
            copy_out(GTA7LITE_CONNSYS_EMI + (address & 0xffffff), payload, length);
            continue;
        }
        uint32_t mode = 0x80000000u;
        if (feature & 1) mode |= 1u | (feature & 6u) | 8u | (feature & 0x10 ? 0x40u : 0);
        uint8_t config[12];
        put32(config, address);
        put32(config + 4, length);
        put32(config + 8, mode);
        if (!init_command(1, config, sizeof(config), 1, 0))
            return "Wi-Fi firmware download refused";
        for (uint32_t at = 0; at < length; at += FW_CHUNK) {
            unsigned n = length - at < FW_CHUNK ? length - at : FW_CHUNK;
            if (!tx_command(3, TX3_DESC, packet, init_packet(0, payload + at, n)))
                return "Wi-Fi firmware download stalled";
        }
    }
    uint8_t start[8];
    put32(start, entry ? 1 : 0);
    put32(start + 4, entry);
    if (!init_command(2, start, sizeof(start), 1, 0)) return "Wi-Fi firmware did not start";
    wr(GLO_CFG, rd(GLO_CFG) & ~0x200u);
    if (!wait_bits(CONN_ON_MISC, 6, 6, 2500000, 5000)) return "Wi-Fi firmware not ready";
    return 0;
}

/* A command after the firmware runs: gen4m's 64-byte WIFI_CMD header on TX
 * ring 15 (asicFillCmdTxdInfo), its sequence returned for waiting. */
static int command(uint8_t cid, bool set, const uint8_t *payload, unsigned n) {
    if (n > sizeof(packet) - 64) return -1;
    unsigned length = 64 + n;
    memset(packet, 0, 64);
    put16(packet, length);
    packet[3] = 0x80;
    packet[5] = 0xa0;
    packet[7] = 2;
    put16(packet + 0x20, length - 32);
    packet[0x24] = cid;
    packet[0x25] = 0xa0;
    packet[0x26] = set ? 1 : 0;
    packet[0x27] = ++sequence;
    packet[0x2a] = 3;
    memcpy(packet + 64, payload, n);
    return tx_command(15, TX15_DESC, packet, length) ? packet[0x27] : -1;
}

static void expect(int seq) {
    awaited = (uint8_t)seq;
    answered = false;
}

/* Events and frames until the awaited answer arrives, within timeout_ms. */
static void pump(void (*deliver)(const unsigned char *, unsigned, const struct wifi_rx_status *));

static bool await(unsigned timeout_ms) {
    uint64_t start = time_us();
    while (!answered && time_us() - start < (uint64_t)timeout_ms * 1000) pump(0);
    return answered;
}

const char *gta7lite_wifi_radio_start(const unsigned char mac[6], void (*yield)(void)) {
    started = asleep = channel_held = beacon_lost = sta_record_live = false;
    scan_running = 0;
    sequence = scan_sequence = channel_token = 0;
    job.active = false;
    tokens_out = 0;
    memset(token_used, 0, sizeof(token_used));
    if ((rd(CONN_ON_MISC) & 6) == 6) return "Wi-Fi firmware already running";
    const char *error = hif_init();
    if (!error) error = firmware();
    if (error) return error;
    yield();
    /* The capability query (its answer is event 0xEC), then basic setup, the
     * domain (passive and active channel tables, as rlmDomainSendCmd) and
     * the station's BSS 0 with this MAC (nicActivateNetworkEx). */
    int seq = command(CMD_CAPABILITY, false, 0, 0);
    expect(seq);
    if (seq < 0 || !await(3000) || answered_eid != EVENT_CAPABILITY)
        return "Wi-Fi firmware not answering";
    static const uint8_t basic[12] = {0, 0, 0, 0, 7, 0, 7, 0, 0, 0, 0, 0};
    uint8_t domain[56] = {0};
    static const uint8_t passive[6][5] = {{81, 1, 1, 1, 0},     {82, 1, 1, 14, 0},
                                          {115, 2, 4, 36, 0},   {118, 2, 4, 52, 4},
                                          {121, 2, 4, 100, 12}, {125, 2, 4, 149, 0}};
    static const uint8_t active[6][5] = {{81, 1, 1, 1, 11},   {115, 2, 4, 36, 4},
                                         {118, 2, 4, 52, 4},  {121, 2, 4, 100, 12},
                                         {125, 2, 4, 149, 5}, {0, 0, 0, 0, 0}};
    put16(domain, 'U' << 8 | 'S');
    domain[2] = 1;
    for (unsigned i = 0; i < 6; ++i) memcpy(domain + 4 + 8 * i, passive[i], 5);
    domain[52] = 1;
    bool sent = command(CMD_BASIC_CONFIG, true, basic, sizeof(basic)) >= 0 &&
                command(CMD_DOMAIN, true, domain, sizeof(domain)) >= 0;
    domain[2] = 0;
    for (unsigned i = 0; i < 6; ++i) memcpy(domain + 4 + 8 * i, active[i], 5);
    uint8_t bss[12] = {0, 1, 0, 1};
    memcpy(bss + 4, mac, 6);
    bss[10] = BMC_WLAN;
    sent = sent && command(CMD_DOMAIN, true, domain, sizeof(domain)) >= 0 &&
           command(CMD_BSS_ACTIVATE, true, bss, sizeof(bss)) >= 0;
    if (!sent) return "Wi-Fi firmware setup failed";
    started = true;
    return 0;
}

/* Rates (500 kb/s units) as the firmware's bitmap: 1, 2, 5.5, 11 Mb/s in
 * bits 0-3, 6 to 54 Mb/s in bits 6-13 (wlan_def.h). */
static unsigned rate_bit(unsigned rate) {
    static const uint8_t rates[] = {2, 4, 11, 22, 0, 0, 12, 18, 24, 36, 48, 72, 96, 108};
    for (unsigned i = 0; i < sizeof(rates); ++i)
        if (rates[i] && rates[i] == (rate & 0x7f)) return 1u << i;
    return 0;
}

static void rate_sets(const struct wifi_station *s, uint16_t *rates, uint16_t *basic) {
    *rates = *basic = 0;
    for (unsigned i = 0; i < s->rate_count; ++i) {
        *rates |= (uint16_t)rate_bit(s->rates[i]);
        if (s->rates[i] & 0x80) *basic |= (uint16_t)rate_bit(s->rates[i]);
    }
}

/* The lowest basic rate as a fixed-rate code: CCK 1-11 Mb/s 0x00-0x03 or
 * OFDM 6 Mb/s and up (0x4B, 0x4F, 0x4A, 0x4E, ...: wlan_def.h). */
static unsigned lowest_rate(const struct wifi_station *s) {
    static const uint16_t codes[] = {0x00, 0x01, 0x02, 0x03, 0,    0,    0x4b,
                                     0x4f, 0x4a, 0x4e, 0x49, 0x4d, 0x48, 0x4c};
    uint16_t rates, basic;
    rate_sets(s, &rates, &basic);
    if (!basic) basic = rates;
    for (unsigned i = 0; i < 14; ++i)
        if (basic & 1u << i) return codes[i];
    return s->channel > 14 ? 0x4b : 0x00;
}

static unsigned phy_types(const struct wifi_station *s) {
    uint16_t rates, basic;
    rate_sets(s, &rates, &basic);
    if (s->channel > 14) return 1u << 3;
    return (rates & 0x000f ? 1u << 0 : 0) | (rates & 0x3fc0 ? 1u << 1 : 0);
}

static int send_bss_info(const struct wifi_station *s, bool connected) {
    uint8_t b[116] = {0};
    uint16_t rates, basic;
    rate_sets(s, &rates, &basic);
    b[1] = connected ? 0 : 1;
    b[3] = (uint8_t)s->profile.ssid_length;
    memcpy(b + 4, s->profile.ssid, s->profile.ssid_length);
    if (connected) memcpy(b + 36, s->bssid, 6);
    put16(b + 44, rates);
    put16(b + 46, basic);
    b[48] = connected ? 0 : 0xfe;
    put16(b + 50, lowest_rate(s));
    b[52] = s->channel > 14 ? 3 : (rates & 0x3fc0) ? 1 : 0;
    b[53] = s->profile.security ? 7 : 0; /* WPA2-PSK */
    b[54] = s->profile.security ? 6 : 0; /* CCMP */
    b[55] = (uint8_t)phy_types(s);
    b[58] = BMC_WLAN;
    b[69] = s->channel > 14 ? 2 : 1;
    b[70] = (uint8_t)s->channel;
    b[81] = s->capabilities & 0x20 ? 1 : 0;
    b[82] = (s->capabilities & 0x400) || s->channel > 14 ? 1 : 0;
    put16(b + 86, basic);
    b[88] = b[89] = 1;
    b[90] = 4;
    b[93] = 1;
    return command(CMD_BSS_INFO, true, b, sizeof(b));
}

static int send_sta_record(const struct wifi_station *s, bool associated) {
    uint8_t r[168] = {0};
    uint16_t rates, basic;
    rate_sets(s, &rates, &basic);
    r[1] = 0x41; /* legacy AP */
    memcpy(r + 2, s->bssid, 6);
    put16(r + 8, associated ? s->aid & 0x3fff : 0);
    put16(r + 10, 10);
    r[13] = (uint8_t)phy_types(s);
    put16(r + 14, rates);
    put16(r + 16, basic);
    r[20] = associated ? 2 : 0; /* STATE_3 or STATE_1 */
    r[51] = associated ? 1 : 0; /* answered by event 0x0C */
    r[54] = AP_WLAN;
    r[55] = 0xff;
    r[68] = 3; /* legacy RTS policy */
    put16(r + 74, lowest_rate(s));
    r[92] = r[93] = 64;
    put32(r + 132, 8192);
    return command(CMD_STA_RECORD, true, r, sizeof(r));
}

static int send_channel(const struct wifi_station *s, bool release) {
    uint8_t c[24] = {0};
    c[1] = channel_token;
    c[2] = release ? 1 : 0;
    c[10] = 4;
    if (!release) {
        c[3] = (uint8_t)s->channel;
        c[5] = s->channel > 14 ? 2 : 1;
        put32(c + 12, 4000);
    }
    return command(CMD_CHANNEL, true, c, sizeof(c));
}

static void release_channel(const struct wifi_station *s) {
    if (channel_held && send_channel(s, true) >= 0) channel_held = false;
}

/* A one-channel scan, active with the wildcard SSID (and the network's own
 * for hidden networks), firmware dwell (CMD_SCAN_REQ_V2, 1192 bytes). */
static bool start_scan(const struct wifi_station *s) {
    static uint8_t scan[1192];
    memset(scan, 0, sizeof(scan));
    if (!++scan_sequence) ++scan_sequence;
    scan[0] = scan_sequence;
    scan[2] = 1;
    scan[3] = 1;
    if (!s->survey && s->profile.ssid_length) {
        scan[3] = 4;
        scan[4] = 1;
        put32(scan + 8, s->profile.ssid_length);
        memcpy(scan + 12, s->profile.ssid, s->profile.ssid_length);
    }
    scan[6] = 0x02;
    scan[7] = 1;
    scan[0x9e] = 4;
    scan[0x9f] = 1;
    scan[0xa0] = s->channel > 14 ? 2 : 1;
    scan[0xa1] = (uint8_t)s->channel;
    scan[0x48a] = 1;
    if (command(CMD_SCAN, true, scan, sizeof(scan)) < 0) return false;
    scan_running = scan_sequence;
    return true;
}

static int install_key(const struct wifi_station *s, const struct wifi_key *k) {
    uint8_t c[64] = {0};
    bool pairwise = k->type == WIFI_KEY_PAIRWISE;
    c[0] = 1;
    c[1] = pairwise;
    c[2] = pairwise;
    memcpy(c + 4, s->bssid, 6);
    c[11] = 4; /* CCMP */
    c[12] = (uint8_t)(pairwise ? 0 : k->index);
    c[13] = 16;
    c[14] = pairwise ? AP_WLAN : BMC_WLAN;
    memcpy(c + 16, k->key, 16);
    int seq = command(CMD_KEY, true, c, sizeof(c));
    /* The key stays only in the DMA cell until the ring reuses it. */
    memset(c, 0, sizeof(c));
    memset(packet + 64, 0, sizeof(c));
    return seq;
}

static int finish(int result) {
    job.active = false;
    return result;
}

int gta7lite_wifi_radio_configure(enum wifi_action action, const struct wifi_station *s,
                                  uint64_t now) {
    if (!started) return -1;
    if (asleep) return 0; /* wifi.c wakes it for a station with work */
    if (!job.active || job.action != action)
        job = (struct radio_job){.action = action, .active = true};
    switch (action) {
        case WIFI_SCAN_INIT:
        case WIFI_SCAN_FINISH:
        case WIFI_LINK: return finish(1);
        case WIFI_SCAN_START: return finish(start_scan(s) ? 1 : -1);
        case WIFI_SCAN_END:
            /* A scan still running is cancelled; done or not, 100 ms later
             * the channel is free again (scnFsmMsgAbort). */
            if (!scan_running || scan_done_sequence == scan_running) {
                scan_running = 0;
                return finish(1);
            }
            if (!job.step) {
                uint8_t cancel[4] = {(uint8_t)scan_running};
                if (command(CMD_SCAN_CANCEL, true, cancel, sizeof(cancel)) < 0) return finish(-1);
                job.step = 1;
                job.deadline = now + 100000;
            }
            if (now < job.deadline) return 0;
            scan_running = 0;
            return finish(1);
        case WIFI_JOIN:
            /* The BSS's record, then the channel granted (cnmChMngrRequestPrivilege). */
            if (!job.step) {
                if (!++channel_token) ++channel_token;
                granted = false;
                if (send_bss_info(s, false) < 0 || send_channel(s, false) < 0) return finish(-1);
                channel_held = true;
                job.step = 1;
                job.deadline = now + 1000000;
            }
            if (granted && grant[1] == channel_token) return finish(grant[2] ? -1 : 1);
            return now < job.deadline ? 0 : finish(-1);
        case WIFI_BSS_ADD:
            if (send_sta_record(s, false) < 0) return finish(-1);
            sta_record_live = true;
            return finish(1);
        case WIFI_BSS_UPDATE: return finish(send_bss_info(s, true) < 0 ? -1 : 1);
        case WIFI_STA:
            if (!job.step) {
                int seq = send_sta_record(s, true);
                if (seq < 0) return finish(-1);
                expect(seq);
                job.step = 1;
                job.deadline = now + 1000000;
            }
            if (answered) {
                if (!s->auth) release_channel(s);
                return finish(1);
            }
            return now < job.deadline ? 0 : finish(-1);
        case WIFI_INSTALL_KEY: {
            const struct wifi_key *k = s->install_key;
            if (!k) return finish(-1);
            if (k->type == WIFI_KEY_INTEGRITY) return finish(1); /* verified by the station */
            if (!job.step) {
                pkey_done = false;
                if (install_key(s, k) < 0) return finish(-1);
                job.step = 1;
                job.deadline = now + 1000000;
            }
            if (k->type == WIFI_KEY_GROUP) {
                /* The handshake is over: the join's channel is released. */
                release_channel(s);
                return finish(1);
            }
            if (pkey_done) return finish(1);
            return now < job.deadline ? 0 : finish(-1);
        }
        case WIFI_LINK_IDLE: release_channel(s); return finish(1);
        case WIFI_DELETE_STA: {
            static const uint8_t remove[4] = {0, 0, 0, 0};
            sta_record_live = false;
            return finish(command(CMD_STA_REMOVE, true, remove, sizeof(remove)) < 0 ? -1 : 1);
        }
        case WIFI_DELETE_BSS: {
            static const uint8_t abort_pm[4] = {0};
            release_channel(s);
            return finish(command(CMD_PM_ABORT, true, abort_pm, sizeof(abort_pm)) < 0 ||
                                  send_bss_info(s, false) < 0
                              ? -1
                              : 1);
        }
    }
    return finish(-1);
}

/* A management frame behind its TXD on TX ring 15 (nicTxSetMngPacket): MCU
 * port, queue 1, 802.11 header format, the lowest basic rate fixed,
 * lifetime 61 x 32 TU, 30 retries, FORCE_TX, its own sequence number. */
static bool send_management(const uint8_t *f, unsigned n) {
    if (n < 24 || n > sizeof(packet) - 32) return false;
    uint32_t dw[8] = {0};
    unsigned wlan = sta_record_live ? AP_WLAN : NO_STA_WLAN;
    dw[0] = (32 + n) | 14u << 16 | 1u << 26 | 1u << 31;
    dw[1] = wlan | 12u << 8 | 2u << 13 | 1u << 15 | 2u << 24 | 1u << 26;
    dw[2] = (uint32_t)(f[0] >> 4 & 0xf) | (uint32_t)(f[0] >> 2 & 3) << 4 | 0x3du << 16 | 1u << 31;
    dw[3] = 1u << 4 | 30u << 11 | (uint32_t)(le16(f + 22) >> 4) << 16 | 1u << 31;
    if (!++next_pid || next_pid > 127) next_pid = 1;
    dw[5] = next_pid | 1u << 9;
    for (unsigned i = 0; i < 8; ++i) put32(packet + 4 * i, dw[i]);
    memcpy(packet + 32, f, n);
    return tx_command(15, TX15_DESC, packet, 32 + n);
}

/* A data frame as Ethernet on TX ring 0 (halWpdmaWriteMsdu): a token's
 * buffer holds the TXD, the TXP pointing at the frame, then the frame;
 * the hardware builds the 802.11 header and, with PF set, encrypts with the
 * entry's key. EAPOL frames go at the lowest rate with a TX status. */
static bool send_data(const uint8_t *f, unsigned n, unsigned rate) {
    if (n < 32 || n - 32 + 14 > 1514) return false;
    unsigned token = 0;
    while (token < TOKENS && token_used[token]) ++token;
    if (token == TOKENS) return false;
    uint32_t idx = rd(ring(TX_RING, 0, 2));
    if (idx >= TX_CELLS || (idx + 1) % TX_CELLS == rd(ring(TX_RING, 0, 3))) return false;
    static uint8_t frame[64 + 1514];
    unsigned length = n - 32 + 14;
    uintptr_t buffer = TOKEN_BUF + token * TOKEN_BUF_SIZE;
    bool eapol = f[30] == 0x88 && f[31] == 0x8e, protect = f[1] & 0x40;
    uint32_t dw[8] = {0};
    dw[0] = (32 + length) | 8u << 16 | 1u << 26; /* LMAC, best effort */
    dw[1] = AP_WLAN | 1u << 12 | 1u << 15 | 1u << 26;
    dw[3] = 30u << 11 | (protect ? 1u << 1 : 0);
    if (eapol) {
        dw[2] = 1u << 31;
        if (!++next_pid || next_pid > 127) next_pid = 1;
        dw[5] = next_pid | 1u << 9;
        dw[6] = (uint32_t)rate << 16;
    }
    memset(frame, 0, 64);
    for (unsigned i = 0; i < 8; ++i) put32(frame + 4 * i, dw[i]);
    put16(frame + 0x20, token | 0x8000);
    put32(frame + 0x28, (uint32_t)(buffer + 64));
    put16(frame + 0x2c, length | 0x8000);
    memcpy(frame + 64, f + 16, 6);      /* destination: address 3 */
    memcpy(frame + 70, f + 10, 6);      /* source: address 2 */
    memcpy(frame + 76, f + 30, n - 30); /* EtherType and payload */
    copy_out(buffer, frame, 64 + length);
    token_used[token] = true;
    ++tokens_out;
    uintptr_t d = TX0_DESC + 16 * idx;
    wr(d + 0, (uint32_t)buffer);
    wr(d + 8, 0);
    wr(d + 12, 0);
    wr(d + 4, 0x40000000u | 64u << 16);
    dma_barrier();
    wr(ring(TX_RING, 0, 2), (idx + 1) % TX_CELLS);
    return true;
}

bool gta7lite_wifi_radio_send(const void *frame, unsigned size, bool data) {
    const uint8_t *f = frame;
    if (!started || asleep || !f) return false;
    if (!data) return send_management(f, size);
    /* EAPOL goes at the network's lowest rate. */
    return send_data(f, size, lowest_rate(wifi_device_station()));
}

/* A received data frame, header-translated: [DA][SA][EtherType] with the
 * original frame control, transmitter, sequence and QoS fields in group 4;
 * rebuilt as the FromDS 802.11 frame the station expects. */
static unsigned rebuild(const uint8_t *p, unsigned at, unsigned total, unsigned g4,
                        struct wifi_rx_status *status) {
    if (!g4 || at + 14 > total || total - at - 14 + 34 > sizeof(rebuilt)) return 0;
    uint32_t dw1 = le32(p + 4), dw2 = le32(p + 8);
    unsigned fc = le16(p + g4) & ~0x8000u, header = fc & 0x80 ? 26 : 24;
    put16(rebuilt, fc);
    put16(rebuilt + 2, 0);
    memcpy(rebuilt + 4, p + at, 6);
    memcpy(rebuilt + 10, p + g4 + 2, 6);
    memcpy(rebuilt + 16, p + at + 6, 6);
    put16(rebuilt + 22, le16(p + g4 + 8));
    if (header == 26) put16(rebuilt + 24, le16(p + g4 + 10) & ~0x0080u);
    static const uint8_t snap[6] = {0xaa, 0xaa, 0x03, 0, 0, 0};
    memcpy(rebuilt + header, snap, 6);
    memcpy(rebuilt + header + 6, p + at + 12, total - at - 12);
    unsigned errors = dw2 >> 16 & ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5));
    status->decrypted = (dw2 >> 12 & 0xf) && !errors;
    status->management = false;
    status->key_index = dw1 >> 6 & 3;
    status->data_offset = header;
    return header + 6 + total - at - 12;
}

static void pump(void (*deliver)(const unsigned char *, unsigned, const struct wifi_rx_status *)) {
    for (unsigned r = 0; r < 2; ++r) {
        unsigned n = rx_take(r);
        if (n < 8) continue;
        uint32_t dw0 = le32(rx_buffer), dw1 = le32(rx_buffer + 4);
        unsigned type = dw0 >> 29, sw = dw0 >> 16 & 0xe00f, total = dw0 & 0xffff;
        if (total > n) total = n;
        if (type == 6) {
            /* MSDU report: the tokens of frames sent (halRxProcessMsduReport),
             * in the format version 1 this firmware uses: after two words,
             * a word per token, 12 bytes for one. */
            unsigned count = dw0 >> 16 & 0x7f;
            for (unsigned i = 0; i < count; ++i) {
                unsigned at = 8 + 4 * i;
                if (at + 2 > total) break;
                unsigned token = le16(rx_buffer + at) & 0x7fff;
                if (token < TOKENS && token_used[token]) {
                    token_used[token] = false;
                    --tokens_out;
                }
            }
            continue;
        }
        if (n < 16) continue;
        if (type == 7 && sw == 0xe000) {
            if (total < 16 + 12) continue;
            const uint8_t *e = rx_buffer + 16;
            unsigned eid = e[4];
            if (eid == EVENT_CHANNEL && total >= 16 + 12 + 24) {
                memcpy(grant, e + 12, 24);
                granted = true;
            }
            if (eid == EVENT_PKEY_DONE) pkey_done = true;
            if (eid == EVENT_SCAN_DONE) scan_done_sequence = e[12];
            if (eid == EVENT_BEACON_TIMEOUT) beacon_lost = true;
            if (!answered && e[5] == awaited) {
                answered = true;
                answered_eid = (uint8_t)eid;
            }
            continue;
        }
        if (!deliver || (type != 2 && !(type == 7 && sw == 0xe001))) continue;
        unsigned groups = dw0 >> 25 & 0xf, at = 16, g4 = 0;
        if (groups & 8) {
            g4 = at;
            at += 16;
        }
        if (groups & 1) at += 16;
        if (groups & 2) at += 8;
        if (groups & 4) at += 24;
        if (dw1 & (1u << 22)) at += 2;
        if (at >= total) continue;
        struct wifi_rx_status status = {0};
        if (dw1 & (1u << 23)) {
            unsigned length = rebuild(rx_buffer, at, total, g4, &status);
            if (length) deliver(rebuilt, length, &status);
        } else if (type == 7) {
            status.management = true;
            deliver(rx_buffer + at, total - at, &status);
        }
    }
}

void gta7lite_wifi_radio_poll(void (*deliver)(const unsigned char *, unsigned,
                                              const struct wifi_rx_status *)) {
    if (!started || asleep) return;
    for (unsigned i = 0; i < 32; ++i) pump(deliver);
}

bool gta7lite_wifi_radio_drained(void) { return !tokens_out; }

bool gta7lite_wifi_radio_beacon_lost(void) {
    bool lost = beacon_lost;
    beacon_lost = false;
    return lost;
}

/* Ownership of the chip (CONN_HIF_ON_LPCTL): its firmware's lets it sleep, as
 * gen4m returns it when idle (halSetFWOwn); the driver's again within 2 s
 * (halSetDriverOwn). The rings are not touched meanwhile. */
int gta7lite_wifi_radio_sleep(bool sleep) {
    if (!started) return 0;
    if (sleep && !asleep) {
        wr(LPCTL, 1);
        asleep = rd(LPCTL) & 1;
    } else if (!sleep && asleep) {
        wr(LPCTL, 2);
        if (!wait_bits(LPCTL, 1, 0, 2048000, 1000)) return -1;
        asleep = false;
    }
    return asleep;
}
