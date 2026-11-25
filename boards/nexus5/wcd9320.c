#include "nexus5.h"
#include "platform.h"

/* The WCD9320 codec's clock and reset (sound/soc/msm/msm8974.c,
 * drivers/platform/msm/qpnp-clkdiv.c and qpnp-pin.c, drivers/mfd/
 * wcd9xxx-core.c, with the hammerhead device tree): its 9.6 MHz master
 * clock is the PM8941's clock divider 1 (0x5b00: the crystal / 2), out of
 * PM8941 GPIO 15 in its first special function (an output on S3's 1.8 V,
 * CMOS, high drive, no pull); its reset is TLMM GPIO 63, held low 20 ms
 * then released, after which it reports present on SLIMbus. */
enum {
    CLKDIV1 = 0x5b00,
    DIV_CTL1 = 0x43,
    EN_CTL = 0x46,
    XO_DIV_2 = 2,
    ENABLE = 0x80,
    MCLK_GPIO = 0xce00, /* PM8941 GPIO 15 */
    MODE = 0x40,
    SUPPLY = 0x41,
    PULL = 0x42,
    OUTPUT_CTL = 0x45,
    OUTPUT_FUNC1 = 1 << 4 | 2 << 1, /* output, source: special function 1 */
    S3 = 2,
    NO_PULL = 5,
    CMOS_HIGH = 3,
    RESET_GPIO = 63,
    PIN_OUTPUT = 1 << 9,
    DRIVE_2MA = 0,
    PIN_HIGH = 2,
    RESET_US = 20000
};

static bool codec_clock(bool on) {
    if (!on) return nexus5_pmic_write(CLKDIV1 + EN_CTL, 0);
    return nexus5_pmic_write(MCLK_GPIO + SUPPLY, S3) &&
           nexus5_pmic_write(MCLK_GPIO + PULL, NO_PULL) &&
           nexus5_pmic_write(MCLK_GPIO + OUTPUT_CTL, CMOS_HIGH) &&
           nexus5_pmic_write(MCLK_GPIO + MODE, OUTPUT_FUNC1) &&
           nexus5_pmic_write(MCLK_GPIO + EN_CTL, ENABLE) &&
           nexus5_pmic_write(CLKDIV1 + DIV_CTL1, XO_DIV_2) &&
           nexus5_pmic_write(CLKDIV1 + EN_CTL, ENABLE);
}

static void codec_reset(void) {
    mmio_write32(nexus5_gpio(RESET_GPIO) + 4, 0);
    mmio_write32(nexus5_gpio(RESET_GPIO), PIN_OUTPUT | DRIVE_2MA);
    delay_us(RESET_US);
    mmio_write32(nexus5_gpio(RESET_GPIO) + 4, PIN_HIGH);
    delay_us(RESET_US);
}

/* The codec's registers over SLIMbus (wcd9xxx-core.c): byte value elements
 * of its generic device (PGD) at 0x800 + the register. A step writes a
 * register, or changes the bits of mask in it (read, then written if they
 * differ), then waits. */
enum { REGISTERS = 0x800, ALL = 0xff };

struct step {
    uint16_t reg;
    uint8_t value, mask;
    uint16_t wait_us;
};

static uint8_t pgd, ifd; /* the codec's logical addresses */

static bool step(const struct step *s) {
    uint16_t at = (uint16_t)(REGISTERS + s->reg);
    if (s->mask == ALL) {
        if (!nexus5_slimbus_write(pgd, at, s->value)) return false;
    } else {
        uint8_t value;
        if (!nexus5_slimbus_read(pgd, at, &value)) return false;
        uint8_t set = (uint8_t)((value & ~s->mask) | (s->value & s->mask));
        if (set != value && !nexus5_slimbus_write(pgd, at, set)) return false;
    }
    if (s->wait_us) delay_us(s->wait_us);
    return true;
}

static bool run(const struct step *steps, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (!step(&steps[i])) return false;
    return true;
}

#define RUN(steps) run(steps, sizeof(steps) / sizeof(steps[0]))

/* wcd9xxx_bring_up: the codec's digital core out of reset. */
static const struct step bring_up[] = {
    {0x088, 0x04, ALL, 0},
    {0x080, 0x00, ALL, 5000},
    {0x080, 0x03, ALL, 0},
    {0x088, 0x03, ALL, 0},
};

/* What the stock driver writes once at probe for a Taiko 2.0, in its order
 * (taiko_update_reg_defaults with taiko_2_0_reg_defaults, then
 * taiko_codec_init_reg), as the bytes it puts on the bus from the power-on
 * values: the 9.6 MHz master clock and the codec's clock power, the
 * receive paths' defaults, the speaker driver's over-current, clip and
 * current settings, compander 0's, the speaker driver's gain taken from its
 * register, the bias current; with the transmit, headphone, earpiece, line
 * and MAD blocks' defaults, which this route leaves off. */
static const struct step defaults[] = {
    {0x000, 0x02, ALL, 0}, {0x314, 0x03, ALL, 0}, {0x1be, 0x05, ALL, 0}, {0x2b4, 0x78, ALL, 0},
    {0x2bc, 0x78, ALL, 0}, {0x2c4, 0x78, ALL, 0}, {0x2cc, 0x78, ALL, 0}, {0x2d4, 0x78, ALL, 0},
    {0x2dc, 0x78, ALL, 0}, {0x2e4, 0x78, ALL, 0}, {0x2b5, 0xa0, ALL, 0}, {0x2bd, 0xa0, ALL, 0},
    {0x2c5, 0x80, ALL, 0}, {0x2cd, 0x80, ALL, 0}, {0x2d5, 0x80, ALL, 0}, {0x2dd, 0x80, ALL, 0},
    {0x2e5, 0x80, ALL, 0}, {0x150, 0xf1, ALL, 0}, {0x3e0, 0x00, ALL, 0}, {0x3e1, 0x00, ALL, 0},
    {0x3e2, 0x00, ALL, 0}, {0x3e3, 0x03, ALL, 0}, {0x3e4, 0x00, ALL, 0}, {0x3e5, 0x00, ALL, 0},
    {0x3e6, 0x00, ALL, 0}, {0x3e7, 0x00, ALL, 0}, {0x3e8, 0x00, ALL, 0}, {0x3e9, 0x00, ALL, 0},
    {0x3ea, 0x00, ALL, 0}, {0x3eb, 0x40, ALL, 0}, {0x27e, 0x00, ALL, 0}, {0x308, 0x00, ALL, 0},
    {0x30c, 0x00, ALL, 0}, {0x3b2, 0x01, ALL, 0}, {0x153, 0x02, ALL, 0}, {0x155, 0x02, ALL, 0},
    {0x156, 0x44, ALL, 0}, {0x15d, 0x02, ALL, 0}, {0x15f, 0x02, ALL, 0}, {0x160, 0x44, ALL, 0},
    {0x167, 0x02, ALL, 0}, {0x169, 0x02, ALL, 0}, {0x16a, 0x44, ALL, 0}, {0x183, 0xce, ALL, 0},
    {0x186, 0x08, ALL, 0}, {0x18c, 0x51, ALL, 0}, {0x198, 0x10, ALL, 0}, {0x1a5, 0xa4, ALL, 0},
    {0x1a6, 0x7a, ALL, 0}, {0x1aa, 0x69, ALL, 0}, {0x1ac, 0xda, ALL, 0}, {0x1ad, 0x15, ALL, 0},
    {0x1ba, 0x76, ALL, 0}, {0x1c2, 0xc0, ALL, 0}, {0x1c6, 0x78, ALL, 0}, {0x1ce, 0x02, ALL, 0},
    {0x1d2, 0x02, ALL, 0}, {0x1d6, 0x02, ALL, 0}, {0x1da, 0x02, ALL, 0}, {0x1e2, 0x97, ALL, 0},
    {0x1e3, 0x01, ALL, 0}, {0x1e4, 0x00, ALL, 0}, {0x223, 0x48, ALL, 0}, {0x22b, 0x48, ALL, 0},
    {0x233, 0x48, ALL, 0}, {0x23b, 0x48, ALL, 0}, {0x243, 0x48, ALL, 0}, {0x24b, 0x48, ALL, 0},
    {0x253, 0x48, ALL, 0}, {0x25b, 0x48, ALL, 0}, {0x263, 0x48, ALL, 0}, {0x26b, 0x48, ALL, 0},
    {0x2b3, 0x08, ALL, 0}, {0x2bb, 0x08, ALL, 0}, {0x2c3, 0x08, ALL, 0}, {0x2cb, 0x08, ALL, 0},
    {0x2d3, 0x08, ALL, 0}, {0x2db, 0x08, ALL, 0}, {0x2e3, 0x08, ALL, 0}, {0x2fa, 0x00, ALL, 0},
    {0x361, 0x00, ALL, 0}, {0x362, 0x00, ALL, 0}, {0x363, 0x00, ALL, 0}, {0x364, 0x00, ALL, 0},
    {0x365, 0x00, ALL, 0}, {0x36b, 0x37, ALL, 0}, {0x36c, 0x7f, ALL, 0}, {0x1a0, 0xff, ALL, 0},
    {0x1af, 0x01, ALL, 0}, {0x1b5, 0x01, ALL, 0}, {0x1ae, 0x20, ALL, 0}, {0x1b4, 0x20, ALL, 0},
    {0x1cd, 0x20, ALL, 0}, {0x1d1, 0x20, ALL, 0}, {0x1d5, 0x20, ALL, 0}, {0x1d9, 0x20, ALL, 0},
    {0x1e0, 0x04, ALL, 0}, {0x3a3, 0x20, ALL, 0}, {0x3a4, 0x20, ALL, 0}, {0x3a5, 0x20, ALL, 0},
    {0x3a6, 0x20, ALL, 0}, {0x3a7, 0x20, ALL, 0}, {0x3a8, 0x20, ALL, 0}, {0x3a9, 0x40, ALL, 0},
    {0x3aa, 0x40, ALL, 0}, {0x3ab, 0x40, ALL, 0}, {0x3ac, 0x40, ALL, 0}, {0x223, 0x40, ALL, 0},
    {0x22b, 0x40, ALL, 0}, {0x233, 0x40, ALL, 0}, {0x23b, 0x40, ALL, 0}, {0x243, 0x40, ALL, 0},
    {0x24b, 0x40, ALL, 0}, {0x253, 0x40, ALL, 0}, {0x25b, 0x40, ALL, 0}, {0x263, 0x40, ALL, 0},
    {0x26b, 0x40, ALL, 0}, {0x373, 0x37, ALL, 0}, {0x37b, 0x37, ALL, 0}, {0x374, 0x7f, ALL, 0},
    {0x37c, 0x7f, ALL, 0}, {0x1ac, 0xdb, ALL, 0}, {0x1ad, 0x58, ALL, 0}, {0x1a9, 0x1a, ALL, 0},
    {0x1a5, 0x24, ALL, 0}, {0x193, 0xfc, ALL, 0}, {0x104, 0x04, ALL, 0}, {0x3b2, 0x08, ALL, 0},
};

/* The mixer path "speaker" (the hammerhead's mixer_paths.xml): RX7's mixer
 * from SLIMbus RX1, RX7's digital gain at +4 dB (88), the speaker driver's
 * volume 6 of 8; with the port's 16-bit samples (taiko_hw_params). */
static const struct step route[] = {
    {0x38e, 0x05, ALL, 0},
    {0x2e7, 0x04, ALL, 0},
    {0x1e0, 0x14, ALL, 0},
    {0x3ae, 0x02, ALL, 0},
};

/* DAPM's power-up of that path, in its order: compander 0's clock, reset
 * and 48 kHz configuration with the speaker gain from the compander; the
 * receive bias; the master clock (the bandgap in audio mode, then the clock
 * block on the external clock, wcd9xxx-resmgr.c); RX7's interpolator reset
 * and clock, its gain written again once clocked; the speaker driver on. */
static const struct step compander_on[] = {
    {0x310, 0x10, ALL, 0}, {0x309, 0x10, ALL, 0}, {0x309, 0x00, ALL, 0},    {0x1e0, 0x10, ALL, 0},
    {0x368, 0x32, ALL, 0}, {0x369, 0x55, ALL, 0}, {0x36a, 0x01, ALL, 3000}, {0x36a, 0x28, ALL, 0},
    {0x369, 0xb5, ALL, 0}, {0x369, 0xb9, ALL, 0}, {0x1a2, 0x80, ALL, 0},
};

static const struct step clock_on[] = {
    {0x101, 0x80, 0x80, 0},    {0x101, 0x04, 0x04, 0}, {0x101, 0x01, 0x01, 1000},
    {0x101, 0x00, 0x80, 0},    {0x108, 0x00, 0x08, 0}, {0x108, 0x04, 0x0c, 0},
    {0x108, 0x01, 0x01, 1200}, {0x109, 0x00, 0x02, 0}, {0x109, 0x04, 0x04, 0},
    {0x311, 0x01, 0x01, 50},
};

static const struct step speaker_on[] = {
    {0x301, 0x40, ALL, 0}, {0x301, 0x00, ALL, 0}, {0x30f, 0x40, ALL, 0},
    {0x2e7, 0x04, ALL, 0}, {0x1df, 0xef, ALL, 0},
};

/* The reverse, once the channel is gone: the speaker driver off, RX7's
 * clock, compander 0 and the receive bias, then the clock block and the
 * bandgap. */
static const struct step speaker_off[] = {
    {0x1df, 0x6f, ALL, 0},  {0x30f, 0x00, ALL, 0},   {0x368, 0x30, ALL, 0},
    {0x309, 0x10, ALL, 0},  {0x309, 0x00, ALL, 0},   {0x310, 0x00, ALL, 0},
    {0x1e0, 0x14, ALL, 0},  {0x1a2, 0x00, ALL, 0},   {0x109, 0x00, 0x04, 50},
    {0x109, 0x02, 0x02, 0}, {0x108, 0x00, 0x05, 50}, {0x101, 0x00, 0x03, 100},
};

/* The codec's SLIMbus side for SLIM RX1 (wcd9xxx_cfg_slim_sch_rx): its port
 * 16 on the shared channel 144, which the machine driver maps and the DSP's
 * port feeds; the interface device's multi-channel bit for the port and its
 * watermark (12 bytes) with the port enabled; the port connected as the
 * channel's sink; the channel defined and activated: 16-bit isochronous
 * samples at 48 kHz (segment length 4, coefficient 3, root exponent 2, hard
 * isochronous protocol, presence rate 0x83). */
enum {
    RX_PORT = 16,
    CHANNEL = 144,
    IFD_RX_MULTI_CHANNEL = 0x180,
    IFD_RX_PORT_CFG = 0x040,
    PORT_WATERMARK_ENABLE = 2 << 1 | 1
};

static const uint8_t channel_properties[4] = {0x00, 0x24, 0x20, 0x83};

static bool interface_write(uint16_t reg, uint8_t value) {
    return nexus5_slimbus_write(ifd, (uint16_t)(REGISTERS + reg), value);
}

/* The codec found on the bus after its reset (wcd9xxx_slim_probe): its two
 * logical addresses, asked of the DSP every millisecond for up to 100 ms;
 * then its core brought up and its defaults written. */
static bool codec_ready;

static const char *codec_start(void) {
    static const uint8_t pgd_address[6] = {0x00, 0x01, 0xa0, 0x00, 0x17, 0x02};
    static const uint8_t ifd_address[6] = {0x00, 0x00, 0xa0, 0x00, 0x17, 0x02};
    codec_reset();
    bool found = false;
    for (uint64_t start = time_us(); !found && time_us() - start < 100000;) {
        found =
            nexus5_slimbus_address(pgd_address, &pgd) && nexus5_slimbus_address(ifd_address, &ifd);
        if (!found) delay_us(1000);
    }
    if (!found) return "the codec did not appear on SLIMbus";
    uint8_t id[4];
    for (unsigned i = 0; i < 4; ++i)
        if (!nexus5_slimbus_read(pgd, (uint16_t)(REGISTERS + 4 + i), &id[i]))
            return "the codec did not answer";
    /* Taiko 2.0: minor 0x0001, major 0x0102. */
    if (id[0] != 1 || id[1] || id[2] != 2 || id[3] != 1) return "the codec is not a WCD9320 2.0";
    if (!RUN(bring_up) || !RUN(defaults)) return "the codec did not start";
    codec_ready = true;
    return 0;
}

/* The whole path: the bus, the codec, the DSP's port (before the codec's
 * side, as the stock driver's back end prepares it), then DAPM's order. */
const char *nexus5_speaker(bool on) {
    if (!on) {
        bool ok = nexus5_q6_afe_stop();
        ok = nexus5_slimbus_remove(pgd, CHANNEL) && ok;
        ok = RUN(speaker_off) && ok;
        ok = codec_clock(false) && ok;
        ok = nexus5_slimbus_stop() && ok;
        return ok ? 0 : "the speaker did not power down";
    }
    const char *error = nexus5_slimbus_start();
    if (!error && !codec_ready) error = codec_start();
    if (error) return error;
    if (!RUN(route)) return "the codec did not take the speaker route";
    if (!nexus5_q6_afe_start(CHANNEL)) return "the DSP's SLIMbus port did not start";
    if (!RUN(compander_on) || !codec_clock(true) || !RUN(clock_on) || !RUN(speaker_on))
        return "the codec's speaker path did not power up";
    if (!interface_write(IFD_RX_MULTI_CHANNEL, 1) ||
        !interface_write(IFD_RX_PORT_CFG, PORT_WATERMARK_ENABLE) ||
        !nexus5_slimbus_connect_sink(pgd, RX_PORT, CHANNEL) ||
        !nexus5_slimbus_define(pgd, channel_properties, CHANNEL))
        return "the codec's SLIMbus channel did not start";
    return 0;
}
