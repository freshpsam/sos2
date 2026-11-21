#include "gta7lite.h"
#include "platform.h"
#include "wire.h"
#include "runtime_files.h"

/* The Galaxy Tab A7 Lite's touch screen: the touch half of the panel's
 * Novatek NT36523, on MT6765's SPI1 (spi@11010000; stock DT novatek@1),
 * driven as the tablet's own Linux kernel drives it (Samsung's gta7lite
 * kernel: drivers/spi/spi-mt65xx.c with CONFIG_HQ_PROJECT_OT8, and
 * drivers/input/touchscreen/mediatek/novatek_touch). The controller runs its
 * firmware from RAM, sent at every boot: the stock vendor partition's
 * novatek_ts_*_fw.bin for the panel LK found, under /firmware/novatek. */

enum {
    /* SPI1 (spi-mt65xx.c's mt6765: pad select, SCK in CFG2, a 32-byte FIFO). */
    SPI1 = 0x11010000,
    SPI_CFG0 = 0x00,
    SPI_CFG1 = 0x04,
    SPI_TX = 0x10,
    SPI_RX = 0x14,
    SPI_CMD = 0x18,
    SPI_STATUS0 = 0x1c,
    SPI_PAD_SEL = 0x24,
    SPI_CFG2 = 0x28,
    CMD_ACT = 1 << 0,
    CMD_RESUME = 1 << 1,
    CMD_RST = 1 << 2,
    CMD_PAUSE_EN = 1 << 4,
    /* Mode 0, MSB first, little-endian, active-low chip select, finish and
     * pause interrupts on, no DMA and no deassert mode (mtk_spi_hw_init). */
    CMD_MODE_CLEAR = 0xcfe0,
    CMD_MODE_SET = 0x33000,
    /* clk26m: 13 cycles high and low, 1 MHz; chip select set up and held
     * 260 cycles, 10 us (the kernel's OT8 cs_time), idle the most CFG1 has. */
    SPI_SCK_1MHZ = 0x000c000c,
    SPI_CS_SETUP_HOLD = 0x01030103,
    SPI_CS_IDLE = 0xfe,
    /* Clocks (clk-mt6765.c): infracfg's SPI1 gate, off while its status bit
     * is set, and topckgen's spi_sel mux (parent [25:24], 0 = clk26m; gate
     * bit 31; applied by bit 11 of CLK_CFG_UPDATE). */
    INFRACFG = 0x10001000,
    IFR4_CLR = 0xa8,
    IFR4_STA = 0xac,
    IFR_SPI1 = 1 << 6,
    TOPCKGEN = 0x10000000,
    CLK_CFG_UPDATE = 0x004,
    CLK_CFG_2_CLR = 0x68,
    SPI_SEL_UPDATE = 1 << 11,
    /* GPIO 24-31's modes (pinctrl-mt6765.c); SPI1_A is GPIO 25-28 in mode 1,
     * as the stock boot configures them. */
    GPIO_MODE_24 = 0x10005330,
    /* The controller's interrupt line, GPIO 10: an input in GPIO mode, as the
     * stock boot sets it (GPIO 0-31's directions, GPIO 8-15's modes). */
    GPIO_DIR_0 = 0x10005000,
    GPIO_MODE_8 = 0x10005310,
    /* MT6765's external interrupts (eint@1000b000, mtk-eint.c's generic
     * registers), where GPIO 10 is EINT 10: bit 10 of each register's first
     * word. Unmasked, its status bit latches the line's edge until
     * acknowledged; its GIC line (SPI 171) stays disabled. */
    EINT = 0x1000b000,
    EINT_STA = 0x000,
    EINT_ACK = 0x040,
    EINT_MASK_SET = 0x0c0,
    EINT_MASK_CLR = 0x100,
    EINT_SENS_CLR = 0x1c0,
    EINT_POL_SET = 0x340,
    EINT_DOM_EN = 0x400,
    TOUCH_INT = 1 << 10,
    /* A finger is followed while its reports keep coming: none for 100 ms
     * means it lifted and its last report was lost; reports that all fail
     * their checks for 250 ms cancel the gesture, as on the Redmi 2. */
    LIFT_US = 100000,
    LOST_US = 250000,
    /* NT36523 (stock DT, nt36xxx.h, nt36xxx_mem_map.h). */
    NVT_ENG_RST = 0x7fff80,
    NVT_SWRST_N8 = 0x3f0fe,
    NVT_SPI_RD_FAST = 0x3f310,
    NVT_TRIM = 0x3f004,
    EVENT_BUF = 0x2fe00,
    BOOT_RDY = 0x3f10d,
    ILM_LENGTH = 0x3f118,
    ILM_DES = 0x3f128,
    DLM_DES = 0x3f12c,
    DLM_LENGTH = 0x3f130,
    G_ILM_CHECKSUM = 0x3f100,
    G_DLM_CHECKSUM = 0x3f104,
    BLD_CRC_EN = 0x3f30e,
    EVENT_HOST_CMD = 0x50,
    EVENT_RESET_COMPLETE = 0x60,
    EVENT_FWINFO = 0x78,
    NVT_TRANSFER_LEN = 63 * 1024,
    POINT_DATA_LEN = 108,
    MAX_PARTS = 24,
};

static void spi_write(uint32_t offset, uint32_t value) {
    mmio_write32(SPI1 + offset, value);
    barrier();
}

/* A packet's expected time at 1 MHz, then STATUS0 every 20 us: reading it
 * clears it, so reading it back to back as the packet ends can lose the
 * completion. The kernel reads it once, in the interrupt handler. */
static bool spi_wait(unsigned bytes, bool first) {
    delay_us(8 * bytes + (first ? 25 : 5));
    for (uint64_t start = time_us(); time_us() - start < 50000; delay_us(20))
        if (mmio_read32(SPI1 + SPI_STATUS0)) return true;
    return false;
}

/* One chip-select framed transfer of head, then body: 32-byte FIFO packets,
 * the first started by ACT, the rest by RESUME while PAUSE_EN holds chip
 * select. Every start gets a fresh 0-to-1 edge, and a completion left by the
 * last deassert is dropped first. rx, when given, receives every byte. */
static bool spi_frame_once(const uint8_t *head, unsigned head_len, const uint8_t *body,
                           unsigned body_len, uint8_t *rx) {
    unsigned len = head_len + body_len;
    spi_write(SPI_PAD_SEL, 0);
    uint32_t cmd = (mmio_read32(SPI1 + SPI_CMD) & ~(CMD_MODE_CLEAR | CMD_ACT | CMD_RESUME)) |
                   CMD_MODE_SET | CMD_PAUSE_EN;
    spi_write(SPI_CMD, cmd);
    spi_write(SPI_CFG2, SPI_SCK_1MHZ);
    spi_write(SPI_CFG0, SPI_CS_SETUP_HOLD);
    spi_write(SPI_CFG1, (mmio_read32(SPI1 + SPI_CFG1) & ~0xffu) | SPI_CS_IDLE);
    bool ok = true;
    for (unsigned done = 0; ok && done < len;) {
        unsigned n = len - done < 32 ? len - done : 32;
        spi_write(SPI_CFG1, (mmio_read32(SPI1 + SPI_CFG1) & ~0x03ffff00u) | (n - 1) << 16);
        for (unsigned i = 0; i < n; i += 4) {
            uint32_t word = 0;
            for (unsigned j = 0; j < 4 && i + j < n; ++j) {
                unsigned at = done + i + j;
                word |= (uint32_t)(at < head_len ? head[at] : body[at - head_len]) << 8 * j;
            }
            spi_write(SPI_TX, word);
        }
        (void)mmio_read32(SPI1 + SPI_STATUS0);
        if (!done) {
            spi_write(SPI_CMD, cmd | CMD_ACT);
        } else {
            uint32_t now = mmio_read32(SPI1 + SPI_CMD) & ~CMD_RESUME;
            spi_write(SPI_CMD, now);
            spi_write(SPI_CMD, now | CMD_RESUME);
        }
        ok = spi_wait(n, !done);
        if (rx)
            for (unsigned i = 0; i < n; i += 4) {
                uint32_t word = mmio_read32(SPI1 + SPI_RX);
                for (unsigned j = 0; j < 4 && i + j < n; ++j)
                    rx[done + i + j] = (uint8_t)(word >> 8 * j);
            }
        done += n;
        gta7lite_watchdog_feed();
    }
    /* mtk_spi_set_cs's deassert, then mtk_spi_reset. */
    spi_write(SPI_CMD, mmio_read32(SPI1 + SPI_CMD) & ~CMD_PAUSE_EN);
    spi_write(SPI_CMD, mmio_read32(SPI1 + SPI_CMD) | CMD_RST);
    spi_write(SPI_CMD, mmio_read32(SPI1 + SPI_CMD) & ~CMD_RST);
    delay_us(10);
    (void)mmio_read32(SPI1 + SPI_STATUS0);
    return ok;
}

/* The touch driver retries its SPI reads and writes. */
static bool spi_frame(const uint8_t *head, unsigned head_len, const uint8_t *body,
                      unsigned body_len, uint8_t *rx) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (spi_frame_once(head, head_len, body, body_len, rx)) return true;
        delay_us(1000);
    }
    return false;
}

/* Novatek's SPI framing (nt36xxx.c): a write is an address with bit 7 set,
 * then data; a read an address with bit 7 clear, a dummy byte and the data,
 * both within the 128-byte page nvt_set_page selected. */
static uint8_t nvt_tx[POINT_DATA_LEN + 2], nvt_rx[POINT_DATA_LEN + 2];

static bool nvt_write_here(uint32_t addr, const uint8_t *data, unsigned n) {
    uint8_t head = (uint8_t)((addr & 0x7f) | 0x80);
    return spi_frame(&head, 1, data, n, 0);
}

static bool nvt_read_here(uint32_t addr, uint8_t *out, unsigned n) {
    if (n + 2 > sizeof nvt_tx) return false;
    memset(nvt_tx, 0, n + 2);
    nvt_tx[0] = addr & 0x7f;
    if (!spi_frame(nvt_tx, n + 2, 0, 0, nvt_rx)) return false;
    memcpy(out, nvt_rx + 2, n);
    return true;
}

static bool nvt_set_page(uint32_t addr) {
    const uint8_t page[3] = {0xff, (uint8_t)(addr >> 15), (uint8_t)(addr >> 7)};
    return spi_frame(page, 3, 0, 0, 0);
}

static bool nvt_write_addr(uint32_t addr, uint8_t value) {
    return nvt_set_page(addr) && nvt_write_here(addr, &value, 1);
}

static bool nvt_read(uint32_t addr, uint8_t *out, unsigned n) {
    return nvt_set_page(addr) && nvt_read_here(addr, out, n);
}

static void nvt_bootloader_reset(void) {
    nvt_write_addr(NVT_SWRST_N8, 0x69);
    delay_us(5000);
    nvt_write_addr(NVT_SPI_RD_FAST, 0x00);
}

/* The vendor driver's probe order: eng reset, 10 ms, a bootloader reset, then
 * up to five reset-and-read tries of the trim ID, xx xx xx 23 65 03 on an
 * NT36523. */
static bool nt36523_answers(void) {
    nvt_write_addr(NVT_ENG_RST, 0x5a);
    delay_us(1000 + 10000);
    nvt_bootloader_reset();
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        uint8_t id[6] = {0};
        nvt_bootloader_reset();
        if (nvt_read(NVT_TRIM, id, 6) && id[3] == 0x23 && id[4] == 0x65 && id[5] == 0x03)
            return true;
        delay_us(10000);
    }
    return false;
}

/* The firmware file Samsung's driver picks for the panel LK names
 * (nvt_get_tp_module: the first panel name the command line contains). */
static bool contains(const char *text, const char *word) {
    size_t n = strlen(word), m = strlen(text);
    for (size_t i = 0; i + n <= m; ++i)
        if (!memcmp(text + i, word, n)) return true;
    return false;
}

static const char *firmware_for(const char *panel) {
    static const char *const table[][2] = {
        {"nt36523_liansi_hsd_incell", "/firmware/novatek/novatek_ts_pid721f_fw.bin"},
        {"nt36523_hlt_mdt", "/firmware/novatek/novatek_ts_pid7227_fw.bin"},
        {"nt36523bh_qunchuang_inx", "/firmware/novatek/novatek_ts_pid7221_fw.bin"},
        {"nt36523_liansi_hsd_ntfpc", "/firmware/novatek/novatek_ts_ntfpc_pid721f_fw.bin"},
        {"nt36523b_txd_mdt", "/firmware/novatek/novatek_ts_pid724a_fw.bin"},
    };
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; ++i)
        if (contains(panel, table[i][0])) return table[i][1];
    return 0;
}

struct fw_part {
    uint32_t bin, sram, size, crc;
};

static const uint8_t *fw;
static uint32_t fw_size;
static struct fw_part parts[MAX_PARTS];
static unsigned part_count;

/* nt36xxx_fw_update.c's file checks and nvt_bin_header_parser, without the
 * cascade layout no A7 Lite firmware uses: ILM and DLM, then 16-byte info
 * and overlay entries (SRAM address, size - 1, file offset, CRC). */
static const char *fw_parse(void) {
    uint32_t need = 0;
    for (uint32_t i = fw_size / 4096; i > 0 && !need; --i)
        if (!memcmp(fw + i * 4096 - 3, "NVT", 3) || !memcmp(fw + i * 4096 - 3, "MOD", 3))
            need = i * 4096;
    if (!need) return "touch firmware without an end flag";
    if (((fw[need - 4096] + fw[need - 4095]) & 0xff) != 0xff)
        return "touch firmware version broken";
    uint32_t end = le32(fw);
    if (fw[0x20] & 2) return "cascaded touch firmware";
    if (end < 0x30 || end > fw_size) return "touch firmware header broken";
    unsigned info = (end - 0x30 + 0xf) / 0x10;
    unsigned overlays = fw[0x28] & 0x10 ? fw[0x28] & 0x0f : 0;
    part_count = 2 + info + overlays;
    if (part_count > MAX_PARTS) return "touch firmware has too many sections";
    for (unsigned n = 0; n < part_count; ++n) {
        struct fw_part *p = &parts[n];
        if (n < 2) {
            *p = (struct fw_part){le32(fw + n * 12), le32(fw + 4 + n * 12), le32(fw + 8 + n * 12),
                                  le32(fw + 0x18 + n * 4)};
        } else {
            uint32_t at =
                n < 2 + info ? 0x30 + 0x10 * (n - 2) : parts[1].bin + 0x10 * (n - 2 - info);
            if (at > fw_size - 16) return "touch firmware section table broken";
            *p = (struct fw_part){le32(fw + at + 8), le32(fw + at), le32(fw + at + 4),
                                  le32(fw + at + 12)};
        }
        if (p->bin > fw_size || p->size >= fw_size - p->bin) return "touch firmware section broken";
    }
    return 0;
}

/* nvt_set_bld_crc_bank with the NT36523's hw_crc 2: the section's SRAM
 * address, a 3-byte size and the golden CRC, all in one page. */
static void crc_bank(uint32_t des, uint32_t length, uint32_t golden, const struct fw_part *p) {
    const uint8_t a[3] = {(uint8_t)p->sram, (uint8_t)(p->sram >> 8), (uint8_t)(p->sram >> 16)};
    const uint8_t l[3] = {(uint8_t)p->size, (uint8_t)(p->size >> 8), (uint8_t)(p->size >> 16 & 1)};
    const uint8_t c[4] = {(uint8_t)p->crc, (uint8_t)(p->crc >> 8), (uint8_t)(p->crc >> 16),
                          (uint8_t)(p->crc >> 24)};
    nvt_set_page(des);
    nvt_write_here(des, a, 3);
    nvt_write_here(length, l, 3);
    nvt_write_here(golden, c, 4);
}

/* nvt_download_firmware_hw_crc, three tries: each section's size + 1 bytes in
 * writes of up to 63 KiB, CRC checks on, boot ready, then the firmware's
 * reset state, 0xA0-0xAF once it runs. */
static bool download(void) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        nvt_bootloader_reset();
        crc_bank(ILM_DES, ILM_LENGTH, G_ILM_CHECKSUM, &parts[0]);
        crc_bank(DLM_DES, DLM_LENGTH, G_DLM_CHECKSUM, &parts[1]);
        bool sent = true;
        for (unsigned n = 0; sent && n < part_count; ++n)
            for (uint32_t left = parts[n].size ? parts[n].size + 1 : 0, done = 0; sent && left;) {
                uint32_t len = left < NVT_TRANSFER_LEN ? left : NVT_TRANSFER_LEN;
                sent = nvt_set_page(parts[n].sram + done) &&
                       nvt_write_here(parts[n].sram + done, fw + parts[n].bin + done, len);
                done += len;
                left -= len;
            }
        if (!sent) continue;
        uint8_t v = 0;
        nvt_read(BLD_CRC_EN, &v, 1); /* nvt_bld_crc_enable */
        v |= 0x80;
        nvt_write_here(BLD_CRC_EN, &v, 1);
        const uint8_t zero = 0, crc_on = 0xae; /* nvt_fw_crc_enable */
        nvt_set_page(EVENT_BUF | EVENT_RESET_COMPLETE);
        nvt_write_here(EVENT_RESET_COMPLETE, &zero, 1);
        nvt_set_page(EVENT_BUF | EVENT_HOST_CMD);
        nvt_write_here(EVENT_HOST_CMD, &crc_on, 1);
        nvt_write_addr(BOOT_RDY, 1); /* nvt_boot_ready */
        delay_us(5000);
        nvt_set_page(EVENT_BUF | EVENT_RESET_COMPLETE);
        for (unsigned retry = 0; retry <= 10; ++retry) {
            uint8_t state[5] = {0};
            nvt_read_here(EVENT_RESET_COMPLETE, state, 5);
            if (state[0] >= 0xa0 && state[0] <= 0xaf) return true;
            delay_us(10000);
        }
    }
    return false;
}

static struct {
    bool ready, down;
    unsigned id; /* the followed finger, 1-10 */
    uint32_t x, y, max_x, max_y, width, height;
    uint64_t edge, report; /* when the last edge latched, and the last good report */
} touch;

const char *gta7lite_touch_init(const char *panel, uint32_t width, uint32_t height) {
    memset(&touch, 0, sizeof touch);
    const char *name = firmware_for(panel);
    if (!name) return "no touch firmware for this panel";
    const struct runtime_file *file = runtime_file_find(name);
    if (!file) return "touch firmware missing from this build";
    fw = runtime_file_data(file);
    fw_size = file->size;
    const char *error = fw_size >= 4096 ? fw_parse() : "touch firmware too short";
    if (error) return error;
    if ((mmio_read32(GPIO_MODE_24) & 0x000ffff0) != 0x00011110) return "SPI1 pins not set up";
    if ((mmio_read32(GPIO_MODE_8) & 0xf00) || (mmio_read32(GPIO_DIR_0) & TOUCH_INT))
        return "touch interrupt pin not set up";
    mmio_write32(TOPCKGEN + CLK_CFG_2_CLR, 3u << 24); /* spi_sel: clk26m */
    mmio_write32(TOPCKGEN + CLK_CFG_2_CLR, 1u << 31); /* ungated */
    mmio_write32(TOPCKGEN + CLK_CFG_UPDATE, SPI_SEL_UPDATE);
    mmio_write32(INFRACFG + IFR4_CLR, IFR_SPI1);
    barrier();
    delay_us(100);
    if (mmio_read32(INFRACFG + IFR4_STA) & IFR_SPI1) return "SPI1 clock stays off";
    if (!nt36523_answers()) return "touch controller not answering";
    if (!download()) return "touch firmware did not start";
    uint8_t info[38] = {0}; /* nvt_get_fw_info: version, its complement, ..., the range */
    if (nvt_read(EVENT_BUF | EVENT_FWINFO, info, sizeof info) &&
        ((info[0] + info[1]) & 0xff) == 0xff) {
        touch.max_x = (uint32_t)info[4] << 8 | info[5];
        touch.max_y = (uint32_t)info[6] << 8 | info[7];
    }
    if (!touch.max_x || !touch.max_y) touch.max_x = width, touch.max_y = height;
    nvt_set_page(EVENT_BUF); /* reports are read from offset 0 of this page */
    /* Reports are readable after the line's rising edge, where Samsung's
     * driver reads one (INT_TRIGGER_TYPE): latch that edge (mtk_eint_set_type,
     * with mtk_eint_hw_init's domain enable). */
    mmio_write32(EINT + EINT_MASK_SET, TOUCH_INT);
    mmio_write32(EINT + EINT_SENS_CLR, TOUCH_INT);
    mmio_write32(EINT + EINT_POL_SET, TOUCH_INT);
    mmio_write32(EINT + EINT_DOM_EN, mmio_read32(EINT + EINT_DOM_EN) | TOUCH_INT);
    mmio_write32(EINT + EINT_ACK, TOUCH_INT);
    mmio_write32(EINT + EINT_MASK_CLR, TOUCH_INT);
    touch.width = width;
    touch.height = height;
    touch.ready = true;
    return 0;
}

static uint32_t scale(uint32_t value, uint32_t max, uint32_t size) {
    uint32_t v = value >= max ? size - 1 : value * (size - 1) / max;
    return v < size ? v : size - 1;
}

/* POINT_DATA_CHECKSUM: the first 64 bytes and their two's complement; and not
 * nvt_fw_recovery's six 0x77 bytes. */
static bool report_valid(const uint8_t *report) {
    bool handshake = true;
    for (unsigned i = 0; i < 6; ++i) handshake = handshake && report[i] == 0x77;
    uint8_t sum = 0;
    for (unsigned i = 0; i < 64; ++i) sum = (uint8_t)(sum + report[i]);
    return !handshake && (uint8_t)(~sum + 1) == report[64];
}

/* Ten 6-byte slots: id[7:3] and status[2:0], x[11:4], y[11:4], x[3:0] y[3:0],
 * width, pressure. Statuses 1, 2, 5, 6 and 7 are down (enter, move, palm,
 * glove, metal jig); the first finger down is followed until it lifts. */
static bool follow(const uint8_t *report, struct pointer_event *event) {
    for (unsigned i = 0; i < 10; ++i) {
        const uint8_t *slot = report + 6 * i;
        unsigned id = slot[0] >> 3, status = slot[0] & 7;
        bool pressed = status == 1 || status == 2 || status == 5 || status == 6 || status == 7;
        if (!id || !pressed || (touch.down && id != touch.id)) continue;
        uint32_t x = scale((uint32_t)slot[1] << 4 | slot[3] >> 4, touch.max_x, touch.width);
        uint32_t y = scale((uint32_t)slot[2] << 4 | (slot[3] & 15), touch.max_y, touch.height);
        if (touch.down && x == touch.x && y == touch.y) return false;
        touch.down = true;
        touch.id = id;
        touch.x = x;
        touch.y = y;
        *event = (struct pointer_event){x, y, true, false};
        return true;
    }
    if (!touch.down) return false;
    touch.down = false;
    *event = (struct pointer_event){touch.x, touch.y, false, false};
    return true;
}

/* One report per edge, as Samsung's driver reads them: reports read on a
 * timer instead mostly fail their checksum, and a lift's report goes unread. */
bool gta7lite_touch_poll(struct pointer_event *event) {
    if (!touch.ready) return false;
    uint64_t now = time_us();
    if (mmio_read32(EINT + EINT_STA) & TOUCH_INT) {
        /* Acknowledged first, so an edge during the read latches again. */
        mmio_write32(EINT + EINT_ACK, TOUCH_INT);
        touch.edge = now;
        uint8_t report[POINT_DATA_LEN];
        if (nvt_read_here(0, report, sizeof report) && report_valid(report)) {
            touch.report = now;
            return follow(report, event);
        }
    }
    if (!touch.down) return false;
    bool lifted = now - touch.edge >= LIFT_US;
    if (!lifted && now - touch.report < LOST_US) return false;
    touch.down = false;
    *event = (struct pointer_event){touch.x, touch.y, false, !lifted};
    return true;
}

void gta7lite_touch_standby(bool active) {
    if (!touch.ready) return;
    if (active) touch.down = false;
    else mmio_write32(EINT + EINT_ACK, TOUCH_INT);
}
