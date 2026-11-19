/* The connectivity subsystem (CONNSYS: Wi-Fi, Bluetooth and GPS inside the
 * MT6765, with the MT6631 radio chip), started as MediaTek's mt6765 WMT
 * driver and wmt_launcher start it (wmt_drv.ko, mtk_wcn_consys_hw.c,
 * mt6765.c, wmt_ic_soc.c, stp_core.c; checked against Android's log): the
 * supplies, the radio chip's pins, the power domain, the subsystem's memory
 * with its MCU, Wi-Fi and Bluetooth RAM code, the CPU out of reset; then over
 * BTIF in STP framing the ROM patch, the WMT reset and setup, and RF
 * calibration. Wi-Fi itself then runs over its own DMA rings (wifi_radio.c).
 * Everything is polled. */
#include "gta7lite.h"
#include "platform.h"
#include "wire.h"
#include "runtime_files.h"
#include <string.h>

enum {
    /* The radio chip's interface pins and their driving (mt6765.h) */
    GPIO = 0x10005000,
    DRIVING = 0x10002c00,
    INFRACFG = 0x10001000,
    PROTECTEN_STA1 = 0x228,
    PROTECTEN_CLR = 0x2a4,
    PROTECTEN_STA1_1 = 0x258,
    PROTECTEN_1_CLR = 0x2ac,
    EMI_MAPPING = 0x380,      /* the subsystem's view of DRAM: base >> 24 */
    EMI_PERI_MAPPING = 0x388, /* and of the peripherals */
    IFR_CG0_CLR = 0x84,
    IFR_CG0_STA = 0x90,
    IFR_BTIF = 1u << 31,
    SPM = 0x10006000,
    POWERON_CONFIG_EN = 0x000,
    PWR_STATUS = 0x180,
    PWR_STATUS_2ND = 0x184,
    CONN_PWR_CON = 0x32c,
    PWR_RST_B = 1u << 0,
    PWR_ISO = 1u << 1,
    PWR_ON = 1u << 2,
    PWR_ON_2ND = 1u << 3,
    PWR_CLK_DIS = 1u << 4,
    CONN_STA = 1u << 1,
    TOPRGU_SWSYSRST = 0x10007018,
    CONN_CPU_RESET = 1u << 12,
    RGU_KEY = 0x88u << 24,
    CONN_MCU = 0x18002000,
    CONN_TOP_MISC_OFF = 0x180b1000,
    IP_VERSION = 0x10020300,
    ROM_DELSEL = 0x114,
    SW_IRQ = 0x150,
    EMI_CTRL = (1u << 0) | (1u << 15) | (1u << 21),
    ROM_WORD = 0x600,
    ROM_READY = 0x1d1e,
    RAM_CODE_HEADER = 48, /* struct wmt_rom_patch */
    /* BTIF (btif_priv.h), polled: no interrupts, no DMA */
    BTIF = 0x1100c000,
    BTIF_DATA = 0x00,
    BTIF_IER = 0x04,
    BTIF_FIFOCTRL = 0x08,
    BTIF_FAKELCR = 0x0c,
    BTIF_LSR = 0x14,
    BTIF_DMA_EN = 0x4c,
    BTIF_TRI_LVL = 0x60,
    BTIF_WAK = 0x64,
    BTIF_HANDSHAKE = 0x6c,
    LSR_DR = 1u << 0,
    LSR_TEMT = 1u << 6,
    STP_WMT = 4,
    PATCH_HEADER = 28 + 2, /* struct WMT_PATCH and its checksum */
    PACE_US = 100,         /* between 16-byte bursts into BTIF (btif_put) */
    SETTLE_US = 20000,     /* after an answer, before the next command (settle) */
};

static const char *const ram_codes[] = {"/firmware/connsys/soc1_0_ram_mcu_1_1_hdr.bin",
                                        "/firmware/connsys/soc1_0_ram_wifi_1_1_hdr.bin",
                                        "/firmware/connsys/soc1_0_ram_bt_1_1_hdr.bin"};
static bool reserved;

static uint32_t rd(uintptr_t a) { return mmio_read32(a); }
static void wr(uintptr_t a, uint32_t v) { mmio_write32(a, v); }


static bool overlap(uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size) {
    return a_size && b_size && a < b + b_size && b < a + a_size;
}

/* The subsystem's memory and Wi-Fi's DMA pool where Linux would put the
 * device tree's consys and wifi reservations (16 MiB-aligned, below
 * 0xC0000000), kept out of every later allocation. */
bool gta7lite_connsys_reserve(struct platform_devices *d, uint64_t kernel_start, uint64_t kernel_end,
                              uint64_t dtb, uint64_t dtb_size) {
    static const struct memory_region regions[] = {{GTA7LITE_CONNSYS_EMI, GTA7LITE_CONNSYS_EMI_SIZE},
                                                   {GTA7LITE_WIFI_POOL, GTA7LITE_WIFI_POOL_SIZE}};
    reserved = false;
    if (d->reserved_region_count + 2 > MAX_RESERVED_REGIONS) return false;
    for (unsigned i = 0; i < 2; ++i) {
        const struct memory_region *r = &regions[i];
        if (!fdt_range_in_ram(d, r->start, r->size) ||
            overlap(r->start, r->size, kernel_start, kernel_end - kernel_start) ||
            overlap(r->start, r->size, dtb, dtb_size) ||
            overlap(r->start, r->size, (uintptr_t)d->framebuffer.pixels, d->framebuffer_size) ||
            overlap(r->start, r->size, d->initrd_start, d->initrd_end - d->initrd_start))
            return false;
        for (size_t k = 0; k < d->reserved_region_count; ++k)
            if (overlap(r->start, r->size, d->reserved_regions[k].start, d->reserved_regions[k].size))
                return false;
    }
    for (unsigned i = 0; i < 2; ++i) d->reserved_regions[d->reserved_region_count++] = regions[i];
    reserved = true;
    return true;
}

bool gta7lite_connsys_reserved(void) { return reserved; }

/* Bytes into Device memory as aligned words. */
static void copy_words(uintptr_t to, const uint8_t *from, uint32_t n) {
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (unsigned b = 0; b < 4 && i + b < n; ++b) w |= (uint32_t)from[i + b] << 8 * b;
        wr(to + i, w);
    }
}

/* The subsystem's memory, zeroed, with the RAM code where wmt_launcher puts
 * it: the header's address without its low byte, which wmt_launcher clears
 * (11 00 08 f0: offset 0x80000), then the remap registers. */
static const char *memory(void) {
    for (uint32_t i = 0; i < GTA7LITE_CONNSYS_EMI_SIZE; i += 4) wr(GTA7LITE_CONNSYS_EMI + i, 0);
    for (unsigned i = 0; i < sizeof(ram_codes) / sizeof(ram_codes[0]); ++i) {
        const struct runtime_file *f = runtime_file_find(ram_codes[i]);
        if (!f || f->size <= RAM_CODE_HEADER) return "Wi-Fi RAM code missing";
        const uint8_t *data = runtime_file_data(f);
        uint32_t offset = le32(data + 24) & 0xffff00u, size = f->size - RAM_CODE_HEADER;
        if (data[22] != 0x8a || offset + size > GTA7LITE_CONNSYS_EMI_SIZE) return "Wi-Fi RAM code invalid";
        copy_words(GTA7LITE_CONNSYS_EMI + offset, data + RAM_CODE_HEADER, size);
    }
    wr(INFRACFG + EMI_MAPPING,
       (rd(INFRACFG + EMI_MAPPING) & 0xfffff000u) | (GTA7LITE_CONNSYS_EMI >> 24 & 0xfff));
    wr(INFRACFG + EMI_PERI_MAPPING,
       (rd(INFRACFG + EMI_PERI_MAPPING) & 0xf000ffffu) | ((0x10003000u >> 24 & 0xfff) << 16));
    return 0;
}

/* Power-on as mtk_wcn_consys_hw_reg_ctrl(1) for mt6765, with the memory in
 * place first, as wmt_launcher leaves it before the power-on. */
static const char *power_on(void) {
    const char *error = memory();
    if (error) return error;
    if (!gta7lite_connsys_supplies()) return "Wi-Fi supplies failed";
    /* The radio chip's interface pins (consys_set_if_pinmux) */
    wr(GPIO + 0x3f0, (rd(GPIO + 0x3f0) & 0xffffff88u) | 0x00000011u);
    wr(GPIO + 0x3e0, (rd(GPIO + 0x3e0) & 0x8888888fu) | 0x11111110u);
    wr(DRIVING + 0x0, (rd(DRIVING + 0x0) & 0xc703ffffu) | 0x08240000u);
    wr(DRIVING + 0x10, (rd(DRIVING + 0x10) & 0xfffffff8u) | 0x1u);
    delay_us(150);
    if (!gta7lite_connsys_crystal_supply()) return "Wi-Fi crystal supply failed";
    /* The CPU held in reset, then its power domain (spm_mtcmos_ctrl_conn_pwr)
     * and bus protection off (spm_mtcmos_ctrl_conn_bus_prot). */
    wr(TOPRGU_SWSYSRST, rd(TOPRGU_SWSYSRST) | CONN_CPU_RESET | RGU_KEY);
    wr(SPM + POWERON_CONFIG_EN, (0xb16u << 16) | 1);
    wr(SPM + CONN_PWR_CON, rd(SPM + CONN_PWR_CON) | PWR_ON);
    wr(SPM + CONN_PWR_CON, rd(SPM + CONN_PWR_CON) | PWR_ON_2ND);
    uint64_t start = time_us();
    while (!(rd(SPM + PWR_STATUS) & CONN_STA) || !(rd(SPM + PWR_STATUS_2ND) & CONN_STA))
        if (time_us() - start > 20000) return "Wi-Fi power domain did not start";
    wr(SPM + CONN_PWR_CON, rd(SPM + CONN_PWR_CON) & ~PWR_CLK_DIS);
    wr(SPM + CONN_PWR_CON, rd(SPM + CONN_PWR_CON) & ~PWR_ISO);
    wr(SPM + CONN_PWR_CON, rd(SPM + CONN_PWR_CON) | PWR_RST_B);
    wr(INFRACFG + PROTECTEN_CLR, (1u << 14) | (1u << 16));
    wr(INFRACFG + PROTECTEN_CLR, 1u << 13);
    wr(INFRACFG + PROTECTEN_1_CLR, 1u << 18);
    start = time_us();
    while ((rd(INFRACFG + PROTECTEN_STA1) & ((1u << 13) | (1u << 14) | (1u << 16))) ||
           (rd(INFRACFG + PROTECTEN_STA1_1) & (1u << 18)))
        if (time_us() - start > 10000) return "Wi-Fi bus protection stuck";
    delay_us(10);
    /* The version (polling_consys_chipid), its memory, ROM delay and EMI
     * control, then the CPU out of reset until its ROM is idle. */
    unsigned tries = 0;
    while (rd(CONN_TOP_MISC_OFF + 0x10) != IP_VERSION) {
        if (++tries > 10) return "Wi-Fi subsystem not responding";
        delay_us(20000);
    }
    wr(CONN_MCU + ROM_DELSEL, (rd(CONN_MCU + ROM_DELSEL) & 0xffff0000u) | 0x17);
    wr(CONN_MCU + SW_IRQ, rd(CONN_MCU + SW_IRQ) | EMI_CTRL);
    wr(TOPRGU_SWSYSRST, (rd(TOPRGU_SWSYSRST) & ~CONN_CPU_RESET) | RGU_KEY);
    start = time_us();
    while (rd(CONN_MCU + ROM_WORD) != ROM_READY)
        if (time_us() - start > 500000) return "Wi-Fi ROM did not start";
    return 0;
}

/* BTIF as hal_btif_hw_init leaves it, then the wake pulse. */
static const char *btif_open(void) {
    wr(INFRACFG + IFR_CG0_CLR, IFR_BTIF);
    delay_us(10);
    if (rd(INFRACFG + IFR_CG0_STA) & IFR_BTIF) return "Wi-Fi control link clock off";
    wr(BTIF + BTIF_FAKELCR, 0);
    wr(BTIF + BTIF_HANDSHAKE, rd(BTIF + BTIF_HANDSHAKE) | 1);
    wr(BTIF + BTIF_FIFOCTRL, 0x6);
    wr(BTIF + BTIF_FIFOCTRL, 0);
    wr(BTIF + BTIF_TRI_LVL, 8 | 1u << 4);
    wr(BTIF + BTIF_DMA_EN, 1u << 2);
    wr(BTIF + BTIF_IER, 0);
    for (unsigned i = 0; i < 256 && (rd(BTIF + BTIF_LSR) & LSR_DR); ++i) (void)rd(BTIF + BTIF_DATA);
    wr(BTIF + BTIF_WAK, rd(BTIF + BTIF_WAK) & ~1u);
    delay_us(160);
    wr(BTIF + BTIF_WAK, rd(BTIF + BTIF_WAK) | 1u);
    return 0;
}

/* Bytes into BTIF's TX FIFO, 16 at a time once it is empty, pace_us before
 * each 16. The subsystem's ROM loses bytes that come faster: written as
 * hal_btif_send_data writes (16 bytes into the empty FIFO, then 8 more below
 * its threshold), the 27-byte patch configuration is answered only once more
 * bytes follow, and the patch body loses bytes. */
static bool btif_put(const uint8_t *p, unsigned n, unsigned pace_us) {
    for (unsigned i = 0; i < n;) {
        delay_us(pace_us);
        uint64_t start = time_us();
        while (!(rd(BTIF + BTIF_LSR) & LSR_TEMT))
            if (time_us() - start > 200000) return false;
        for (unsigned k = 0; k < 16 && i < n; ++k) wr(BTIF + BTIF_DATA, p[i++]);
    }
    return true;
}

/* STP: mandatory mode until the WMT setup switches to full mode, where each
 * frame carries a sequence number and an acknowledgement (the last sequence
 * received), a header sum and a CRC-16/ARC, and each data frame received is
 * acknowledged with a header-only frame (stp_core.c). */
static struct {
    bool full;
    unsigned txseq, txack, expected;
} stp;

static uint16_t crc16_arc(const uint8_t *p, unsigned n) {
    uint16_t crc = 0;
    for (unsigned i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8; ++b) crc = crc & 1 ? (uint16_t)(crc >> 1 ^ 0xa001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static bool stp_send(const uint8_t *payload, unsigned n) {
    uint8_t frame[4 + 32 + 2] = {0x80, (uint8_t)(STP_WMT << 4), (uint8_t)n, 0};
    if (n > 32) return false;
    memcpy(frame + 4, payload, n);
    frame[4 + n] = frame[5 + n] = 0;
    if (stp.full) {
        frame[0] = (uint8_t)(0x80 | stp.txseq << 3 | stp.txack);
        frame[3] = (uint8_t)(frame[0] + frame[1] + frame[2]);
        uint16_t crc = crc16_arc(payload, n);
        frame[4 + n] = (uint8_t)crc;
        frame[5 + n] = (uint8_t)(crc >> 8);
        stp.txseq = (stp.txseq + 1) & 7;
    }
    return btif_put(frame, n + 6, PACE_US);
}

/* The next WMT frame's payload within timeout_ms (other frames skipped and,
 * in full mode, acknowledged): its length, 0 on timeout. */
static unsigned stp_receive(uint8_t *out, unsigned capacity, unsigned timeout_ms) {
    unsigned state = 0, length = 0, got = 0, seq = 0, type = 0;
    uint8_t head[4];
    uint64_t start = time_us();
    while (time_us() - start < (uint64_t)timeout_ms * 1000) {
        if (!(rd(BTIF + BTIF_LSR) & LSR_DR)) continue;
        uint8_t c = (uint8_t)rd(BTIF + BTIF_DATA);
        switch (state) {
            case 0:
                if ((c & 0xc0) == 0x80) {
                    head[0] = c;
                    state = 1;
                }
                break;
            case 1: head[1] = c; state = 2; break;
            case 2: head[2] = c; state = 3; break;
            case 3:
                state = 0;
                if (stp.full && (uint8_t)(head[0] + head[1] + head[2]) != c) break;
                length = (head[1] & 0xfu) << 8 | head[2];
                type = head[1] >> 4 & 7;
                seq = head[0] >> 3 & 7;
                got = 0;
                if (length) state = 4;
                break;
            case 4:
                if (got < capacity) out[got] = c;
                if (++got == length) state = 5;
                break;
            case 5: state = 6; break;
            default: {
                state = 0;
                if (stp.full) {
                    if (seq == stp.expected) {
                        stp.txack = seq;
                        stp.expected = (stp.expected + 1) & 7;
                    }
                    uint8_t ack[4] = {(uint8_t)(0x80 | stp.txack), 0, 0, 0};
                    ack[3] = ack[0];
                    (void)btif_put(ack, sizeof(ack), 0);
                }
                if (type == STP_WMT) return got < capacity ? got : capacity;
            }
        }
    }
    return 0;
}

/* Before each command, and the patch body, SETTLE_US after the ROM's last
 * answer: sent at once, the patch configuration is answered but leaves the
 * ROM out of patch mode, so the patch does not take and the WMT reset after
 * it goes unanswered. */
static void settle(void) { delay_us(SETTLE_US); }

/* A WMT command and its event, which must start with `expect`. */
static bool wmt(const uint8_t *command, unsigned n, const uint8_t *expect, unsigned m, unsigned timeout_ms) {
    uint8_t event[64];
    settle();
    if (!stp_send(command, n)) return false;
    unsigned got = stp_receive(event, sizeof(event), timeout_ms);
    return got >= m && !memcmp(event, expect, m);
}

/* The ROM patch by PDA (mtk_wcn_soc_pda_patch_dwn): the configuration with
 * the address wmt_launcher passes (the header's, without its low byte: the
 * patch count and sequence), then raw a 12-byte length header and the body,
 * paced. */
static const char *patch(void) {
    const struct runtime_file *f = runtime_file_find("/firmware/connsys/soc1_0_patch_mcu_1_1_hdr.bin");
    if (!f || f->size <= PATCH_HEADER) return "Wi-Fi ROM patch missing";
    const uint8_t *data = runtime_file_data(f);
    uint32_t size = f->size - PATCH_HEADER;
    uint8_t config[21] = {0x01, 0x01, 0x11, 0x00, 0x00, 0, data[25], data[26], data[27]};
    for (unsigned i = 0; i < 4; ++i) config[9 + i] = (uint8_t)(size >> 8 * i);
    config[17] = 0x2f;
    config[18] = 0x02;
    config[19] = 0x02;
    static const uint8_t accepted[] = {0x02, 0x01, 0x01, 0x00, 0x00};
    if (!wmt(config, sizeof(config), accepted, sizeof(accepted), 1000)) return "Wi-Fi ROM patch refused";
    settle();
    uint8_t head[12] = {0};
    for (unsigned i = 0; i < 4; ++i) head[i] = (uint8_t)((size + 12) >> 8 * i);
    if (!btif_put(head, sizeof(head), PACE_US) || !btif_put(data + PATCH_HEADER, size, PACE_US))
        return "Wi-Fi ROM patch stalled";
    return 0;
}

/* After the patch, mtk_wcn_soc_sw_init: WMT reset, STP full mode, chip ID,
 * the radio chip's ID, RF calibration. */
static const char *setup(void (*yield)(void)) {
    static const uint8_t query[] = {0x01, 0x04, 0x01, 0x00, 0x04}, queried[] = {0x02, 0x04, 0x06, 0x00, 0x00, 0x04};
    static const uint8_t reset[] = {0x01, 0x07, 0x01, 0x00, 0x04}, reset_done[] = {0x02, 0x07, 0x01, 0x00, 0x00};
    static const uint8_t set_stp[] = {0x01, 0x04, 0x05, 0x00, 0x03, 0xdb, 0x0e, 0x68, 0x01},
                         stp_set[] = {0x02, 0x04, 0x02, 0x00, 0x00, 0x03},
                         full_mode[] = {0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0xdb, 0x0e, 0x68, 0x01};
    static const uint8_t chip_id[] = {0x01, 0x02, 0x05, 0x00, 0x11, 0x65, 0x67, 0x00, 0x00},
                         chip_id_set[] = {0x02, 0x02, 0x01, 0x00, 0x00};
    static const uint8_t adie[] = {0x01, 0x02, 0x01, 0x00, 0x12}, adie_found[] = {0x02, 0x02, 0x05, 0x00, 0x00};
    static const uint8_t calibrate[] = {0x01, 0x14, 0x01, 0x00, 0x01}, calibrated[] = {0x02, 0x14};
    if (!wmt(reset, sizeof(reset), reset_done, sizeof(reset_done), 2000)) return "Wi-Fi reset failed";
    yield();
    if (!wmt(query, sizeof(query), queried, sizeof(queried), 500) ||
        !wmt(set_stp, sizeof(set_stp), stp_set, sizeof(stp_set), 500))
        return "Wi-Fi control link setup failed";
    stp.full = true;
    stp.txseq = stp.expected = 0;
    stp.txack = 7;
    if (!wmt(query, sizeof(query), full_mode, sizeof(full_mode), 500)) return "Wi-Fi control link mode failed";
    if (!wmt(chip_id, sizeof(chip_id), chip_id_set, sizeof(chip_id_set), 500)) return "Wi-Fi chip ID refused";
    if (!wmt(adie, sizeof(adie), adie_found, sizeof(adie_found), 500)) return "Wi-Fi radio chip missing";
    yield();
    uint8_t event[64];
    settle();
    if (!stp_send(calibrate, sizeof(calibrate))) return "Wi-Fi calibration not sent";
    unsigned got = stp_receive(event, sizeof(event), 3000);
    if (got < 5 || memcmp(event, calibrated, sizeof(calibrated)) || event[4]) return "Wi-Fi calibration failed";
    return 0;
}

const char *gta7lite_connsys_start(void (*yield)(void)) {
    stp.full = false;
    const char *error = power_on();
    if (!error) error = btif_open();
    if (error) return error;
    yield();
    static const uint8_t query[] = {0x01, 0x04, 0x01, 0x00, 0x04}, queried[] = {0x02, 0x04, 0x06, 0x00, 0x00, 0x04};
    if (!wmt(query, sizeof(query), queried, sizeof(queried), 500)) return "Wi-Fi control link silent";
    error = patch();
    if (error) return error;
    return setup(yield);
}

