#include "audio.h"
#include "platform.h"
#include "redmi2_audio.h"
#include "redmi2_power_hw.h"

/* The Redmi 2's loudspeaker, driven as mainline Linux drives MSM8916 sound
 * without the modem's DSP (sound/soc/qcom/lpass-cpu.c, lpass-platform.c,
 * lpass-apq8016.c and apq8016_sbc.c; sound/soc/codecs/msm8916-wcd-digital.c
 * and msm8916-wcd-analog.c; drivers/clk/qcom/gcc-msm8916.c;
 * msm8916-wingtech-wt88047.dts), with the analog sequences of the WT88047's
 * vendor kernel, whose LPASS that DSP ran (sound/soc/codecs/msm8x16-wcd.c,
 * sound/soc/msm/msm8x16.c), and the speaker route of its mixer paths
 * (LineageOS android_device_wingtech_wt88047, audio/mixer_paths.xml).
 *
 * The audio interface (LPAIF) reads a ring of 16-bit stereo frames in RAM
 * with its first read DMA channel and sends it over the primary MI2S port
 * to the SoC's digital codec; its two receive paths cross the PDM link
 * (GPIO 63-68) to the PM8916's analog codec, whose headphone amplifiers
 * drive the jack through a switch (GPIO 8, left open: nothing senses the
 * jack yet) and, from the right one, the loudspeaker's amplifier, which
 * GPIO 117 enables. As the stock speaker path sends one channel, the
 * loudspeaker gets the mix in mono, at the volume the keys set (12 of 15,
 * -9 dB, at each start), with the digital gains at the 0 dB Android leaves
 * them at. All of it powers up when a stream has frames, the loudspeaker's
 * amplifier last, in about 25 ms, and down 3 s after the last frame, clocks
 * included, so that standby's whole-chip sleep can stop the crystal. The
 * main loop's polls keep the ring filled ahead of the DMA, with silence
 * where the streams have nothing, as the tablet's driver does
 * (boards/gta7lite/audio.c): a poll up to a lap late lets the speaker fall
 * silent rather than replay old samples, and the laps a later one missed are
 * counted from the time that passed. */

enum {
    /* GCC's clocks for the audio block (gcc-msm8916.c): the LPASS bus
     * ports, its AHB fabric, the primary MI2S bit clock and the digital
     * codec's master clock, each a root (command, then configuration, M, N
     * and D) feeding a branch. */
    MPORT_CBCR = 0x0181c000,
    SWAY_CBCR = 0x0181c004,
    FABRIC_RCGR = 0x0181c010,
    FABRIC_CBCR = 0x0181c028,
    I2S_RCGR = 0x0181c054,
    I2S_CBCR = 0x0181c068,
    MCLK_RCGR = 0x0181c09c,
    MCLK_CBCR = 0x0181c0b0,
    RCG_CFG = 0x04,
    RCG_M = 0x08,
    RCG_N = 0x0c,
    RCG_D = 0x10,
    RCG_UPDATE = 1 << 0,
    RCG_FIELDS = 0x1f | 7 << 8 | 3 << 12 | 1 << 20, /* divider, source, mode, hardware control */
    MND_FIELD = 0xff,
    /* lpass-apq8016.c asks 131.072 MHz of the fabric, which its table rounds
     * to its fastest, 100 MHz: GPLL0 / 8. */
    FABRIC_100M = 15 | 1 << 8,
    /* The bit clock, 48 kHz x 2 x 16 bits = 1.536 MHz: the crystal x 2/25,
     * dual-edge (M; N as ~(N - M); D as ~2D, with 2D = N). */
    I2S_1M536 = 1 | 2 << 12,
    I2S_M = 2,
    I2S_N = 0xe8,
    I2S_D = 0xe6,
    /* The codecs' master clock, 9.6 MHz (apq8016_sbc.c): the crystal / 2. */
    MCLK_9M6 = 3,
    CLOCK_OFF = 1u << 31,
    GPLL0_STATUS = 0x0182101c,
    GPLL0_ACTIVE = 1 << 17,
    CLOCK_US = 10000,
    /* LPASS (lpass-apq8016.c, lpass-lpaif-reg.h): the MI2S ports' routing
     * (apq8016_sbc.c's spkr-iomux), the primary port's control and the
     * first read DMA channel. */
    SPKR_IOMUX = 0x07702004,
    PRI_WS_SLAVE_SEL_11 = 3 << 16,
    I2SCTL = 0x07709000,
    SPKEN = 1 << 14,
    I2S_FIELDS = 1 << 15 | 0xf << 10 | 1 << 9 | 1 << 2 | 3, /* loopback, mode, mono, WS, width */
    I2S_SD0 = 1 << 10, /* stereo on SD0, 16 bits, its own word select */
    RDMA = 0x07710400,
    RDMA_CTL = 0x00,
    RDMA_BASE = 0x04,
    RDMA_BUFF = 0x08,
    RDMA_CURR = 0x0c,
    RDMA_PER = 0x10,
    /* lpass-platform.c's setup for 16-bit stereo: INCR4 bursts, one word a
     * frame, interface 1 (the primary port), FIFO watermark 8. */
    RDMA_SETUP = 1 << 11 | 1 << 4 | 7 << 1,
    RDMA_ENABLE = 1 << 0,
    /* The SoC's digital codec (msm8916-wcd-digital.c): byte registers a word
     * apart. */
    CDC = 0x0771c000,
    CDC_RX_RESET = 0x000,
    CDC_RX_I2S = 0x00c,
    RX_I2S_FORMAT = 7 | 1 << 5,
    RX_I2S_48K_16 = 3 | 1 << 5,
    RX_I2S_CLK = 1 << 4,
    CDC_OTHR = 0x01c,
    CDC_CONN = 1 << 2,
    CDC_RX_B1 = 0x020,
    RX1_RX2 = 3, /* RX1's and RX2's interpolators; their resets */
    CDC_MCLK = 0x024,
    MCLK_EN = 1 << 0,
    CDC_PDM = 0x028,
    PDM_EN = 1 << 0,
    PDM_FB = 1 << 1,
    CDC_RX1_MUTE = 0x054, /* RXn_B6_CTL */
    CDC_RX2_MUTE = 0x074,
    RX_MUTE = 1 << 0,
    CDC_RX1_GAIN = 0x05c, /* RXn_VOL_CTL_B2_CTL, in dB */
    CDC_RX2_GAIN = 0x07c,
    CDC_TOP = 0x0a4,
    MCLK_9P6 = 1 << 0,
    CDC_RX1_INPUT = 0x180, /* CONN_RXn_B1_CTL: MIX1's first input */
    CDC_RX2_INPUT = 0x18c,
    INPUT1 = 7,
    FROM_RX1 = 3,
    FROM_RX2 = 4,
    /* The PM8916's analog codec (msm8916-wcd-analog.c, the vendor's
     * msm8x16_wcd_registers.h): SID 1's peripherals 0xf0, its digital part,
     * and 0xf1. */
    PMIC_D = 0x1f0,
    PMIC_A = 0x1f1,
    D_REVISION1 = 0x00,
    D_CDC_RST = 0x46,
    RST_RELEASE = 1 << 7,
    D_TOP_CLK = 0x48,
    A_MCLKS = 3 << 2, /* A_MCLK and A_MCLK2 */
    D_ANA_CLK = 0x49,
    HPHR_CLK = 1 << 0,
    HPHL_CLK = 1 << 1,
    D_DIG_CLK = 0x4a,
    RXD1_CLK = 1 << 0,
    RXD2_CLK = 1 << 1,
    NCP_CLK = 1 << 6,
    RXD_PDM_CLK = 1 << 7,
    D_HPHR_DAC = 0x52,
    RDAC2_RX2 = 1 << 0,
    SEC_ACCESS = 0xd0,
    SEC_UNLOCK = 0xa5,
    RESET_CTL3 = 0xda,
    RESET_CTL4 = 0xdb,
    A_MASTER_BIAS = 0x46,
    A_OPAMP_BIAS = 0x65,
    A_NCP_EN = 0x80,
    NCP_ON = 1 << 0,
    A_NCP_FBCTRL = 0x83,
    FB_CLK_INV = 1 << 5,
    A_OCP_CTL = 0x91,
    A_OCP_COUNT = 0x92,
    A_BIAS_DAC = 0x93,
    DAC_REF = 1 << 0,
    RX_BIAS = 1 << 7,
    A_CNP_EN = 0x97,
    HPH_PAS = 3 << 4, /* HPHL's amplifier (bit 5) and HPHR's (bit 4) */
    A_HPHL_TEST = 0x9a,
    A_HPHL_DAC = 0x9b,
    A_HPHR_TEST = 0x9c,
    A_HPHR_DAC = 0x9d,
    TEST_ON = 1 << 2,
    DAC_ON = 1 << 3,
    DAC_RESET = 1 << 1,
    A_SPKR_DAC = 0xb0,
    A_SPKR_DRV = 0xb2,
    A_SPKR_OCP = 0xb4,
    A_SPKR_DBG = 0xb7,
    A_CURRENT_LIMIT = 0xc0,
    A_BOOST_EN = 0xc3,
    A_SLOPE_COMP = 0xc4,
    ALL = 0xff,
    /* TLMM pins (pinctrl-msm8916.c; the vendor's cdc-pdm-lines and
     * cdc-ext-wt88047-pa states): function 5:2, pull 1:0 (1 down), drive
     * 8:6 (mA/2 - 1), output enable 9; the output level in IN_OUT bit 1. */
    TLMM = 0x01000000,
    GPIO_STRIDE = 0x1000,
    GPIO_IO = 4,
    GPIO_HIGH = 1 << 1,
    PIN_FIELDS = 0x3ff,
    PDM_FIRST = 63,
    PDM_LAST = 68,
    PDM_ON = 3 << 6 | 1 << 2, /* cdc_pdm0, 8 mA, no pull */
    PDM_OFF = 1 << 2 | 1,     /* cdc_pdm0, 2 mA, pulled down */
    SPEAKER_AMP = 117,
    HEADPHONE_SWITCH = 8,
    ENABLE_ON = 1 << 9 | 3 << 6, /* an output, 8 mA */
    ENABLE_OFF = 1 << 9,         /* an output, 2 mA */
    /* The ring: 256 ms, lpass-platform.c's buffer, in two periods for its
     * register (no interrupt is used). Writes start 5 ms ahead of the DMA
     * and fill up to 245 ms ahead. The DMA has stopped when it has not moved
     * for half a second. */
    RING = 12288,
    GUARD = 256,
    LEAD = RING - 2 * GUARD,
    STALL_US = 500000,
    LAP_US = RING * 1000000ull / USER_AUDIO_RATE, /* 256 ms */
    IDLE_US = 3000000,
    MIX_FRAMES = 512,
    SPEAKER_VOLUME = 12
};

/* Static, so uncached and at its physical address (boards/redmi2/kernel.ld,
 * arch/arm/mmu.c), as the DMA reads it: a word a frame, left in the low
 * half. */
static uint32_t ring[RING] __attribute__((aligned(4096)));

static enum { OFF, ON, FAILED } state;

static unsigned volume = SPEAKER_VOLUME;

/* Frame counts since power-up: played by the DMA, as its position moved;
 * audio written; and the ring written up to, audio or silence. read_at is
 * the DMA's position in the ring at the last poll. */
static uint64_t played, written, cleared;
static uint32_t read_at;
static uint64_t moved_at, heard_at, polled_at;

static void update(uintptr_t address, uint32_t value, uint32_t mask) {
    mmio_write32(address, (mmio_read32(address) & ~mask) | (value & mask));
}

static bool wait(uintptr_t address, uint32_t mask, uint32_t value) {
    for (uint64_t start = time_us(); (mmio_read32(address) & mask) != value;)
        if (time_us() - start >= CLOCK_US) return false;
    return true;
}

static bool branch(uintptr_t cbcr, bool on) {
    update(cbcr, on, 1);
    return wait(cbcr, CLOCK_OFF, on ? 0 : CLOCK_OFF);
}

/* clk-rcg2.c's clk_rcg2_configure: the configuration, latched by the update
 * bit (M, N and D before it, for the roots that use them). */
static bool root(uintptr_t rcgr, uint32_t cfg) {
    update(rcgr + RCG_CFG, cfg, RCG_FIELDS);
    mmio_write32(rcgr, mmio_read32(rcgr) | RCG_UPDATE);
    return wait(rcgr, RCG_UPDATE, 0);
}

/* The bus ports and the fabric, which the audio block's registers need,
 * then the codecs' master clock; off in the reverse order. The bit clock
 * follows the port (lpaif_start). */
static bool clocks(bool on) {
    if (!on) {
        bool off = branch(MCLK_CBCR, false);
        off = branch(FABRIC_CBCR, false) && off;
        off = branch(SWAY_CBCR, false) && off;
        return branch(MPORT_CBCR, false) && off;
    }
    return (mmio_read32(GPLL0_STATUS) & GPLL0_ACTIVE) && branch(MPORT_CBCR, true) &&
           branch(SWAY_CBCR, true) && root(FABRIC_RCGR, FABRIC_100M) && branch(FABRIC_CBCR, true) &&
           root(MCLK_RCGR, MCLK_9M6) && branch(MCLK_CBCR, true);
}

static void pin(unsigned gpio, uint32_t cfg) { update(TLMM + gpio * GPIO_STRIDE, cfg, PIN_FIELDS); }

/* An enable: its level, then its drive (before the output turns on, as
 * idle_devices.c sets pins). */
static void enable(unsigned gpio, bool high, uint32_t cfg) {
    mmio_write32(TLMM + gpio * GPIO_STRIDE + GPIO_IO, high ? GPIO_HIGH : 0);
    pin(gpio, cfg);
}

static void pdm_pins(bool on) {
    for (unsigned gpio = PDM_FIRST; gpio <= PDM_LAST; ++gpio) pin(gpio, on ? PDM_ON : PDM_OFF);
}

/* Frames from at on, or silence for frames 0; at counts from power-up. */
static void ring_write(uint64_t at, const struct audio_frame *frames, uint32_t count) {
    uint32_t index = (uint32_t)(at % RING);
    for (uint32_t i = 0; i < count; ++i) {
        ring[index] =
            frames ? (uint16_t)frames[i].left | (uint32_t)(uint16_t)frames[i].right << 16 : 0;
        if (++index == RING) index = 0;
    }
}

/* lpass-platform.c's open, hw_params and prepare for the ring, silent;
 * lpass-cpu.c's hw_params and prepare for the port: stereo on SD0, 16 bits,
 * its own word select, then on, then its bit clock. */
static bool lpaif_start(void) {
    ring_write(0, 0, RING);
    barrier();
    update(I2S_RCGR + RCG_M, I2S_M, MND_FIELD);
    update(I2S_RCGR + RCG_N, I2S_N, MND_FIELD);
    update(I2S_RCGR + RCG_D, I2S_D, MND_FIELD);
    if (!root(I2S_RCGR, I2S_1M536)) return false;
    mmio_write32(RDMA + RDMA_CTL, 0);
    mmio_write32(RDMA + RDMA_CTL, RDMA_SETUP);
    mmio_write32(RDMA + RDMA_BASE, (uint32_t)(uintptr_t)ring);
    mmio_write32(RDMA + RDMA_BUFF, RING - 1);
    mmio_write32(RDMA + RDMA_PER, RING / 2 - 1);
    mmio_write32(RDMA + RDMA_CTL, RDMA_SETUP | RDMA_ENABLE);
    update(I2SCTL, I2S_SD0, I2S_FIELDS);
    update(I2SCTL, SPKEN, SPKEN);
    return branch(I2S_CBCR, true);
}

/* Its trigger stop and hw_free: the port, its bit clock and the DMA off. */
static void lpaif_stop(void) {
    update(I2SCTL, 0, SPKEN);
    (void)branch(I2S_CBCR, false);
    mmio_write32(RDMA + RDMA_CTL, 0);
}

/* msm8916_wcd_digital_startup and hw_params: the master clock in, the PDM
 * clock from its feedback, 9.6 MHz, 48 kHz 16-bit I2S. */
static void digital_start(void) {
    update(CDC + CDC_MCLK, MCLK_EN, MCLK_EN);
    update(CDC + CDC_PDM, PDM_FB, PDM_FB);
    update(CDC + CDC_TOP, MCLK_9P6, MCLK_9P6);
    update(CDC + CDC_RX_I2S, RX_I2S_48K_16, RX_I2S_FORMAT);
}

/* A register of the digital codec (DIGITAL) or of one of the PM8916's two,
 * set under a mask (ALL: written, not read) with a wait after it. */
enum { DIGITAL = 0 };

struct step {
    uint16_t target, reg;
    uint8_t value, mask;
    uint16_t wait_us;
};

/* msm8x16_wcd_bringup, codec_init_reg and the PM8916 2.0 defaults
 * (msm8x16_wcd_reg_defaults_2_0, which mainline's wcd_reg_defaults_2_0
 * also writes, but for the over-current setting): both parts reset,
 * then set up. Writes to the reset controls take an unlock first. */
static const struct step codec_init[] = {
    {PMIC_D, SEC_ACCESS, SEC_UNLOCK, ALL, 0},
    {PMIC_D, RESET_CTL4, 0x01, ALL, 0}, /* the digital part in reset */
    {PMIC_A, SEC_ACCESS, SEC_UNLOCK, ALL, 0},
    {PMIC_A, RESET_CTL4, 0x01, ALL, 0}, /* the analog part */
    {PMIC_D, SEC_ACCESS, SEC_UNLOCK, ALL, 0},
    {PMIC_D, RESET_CTL4, 0x00, ALL, 0}, /* both released */
    {PMIC_A, SEC_ACCESS, SEC_UNLOCK, ALL, 0},
    {PMIC_A, RESET_CTL4, 0x00, ALL, 0},
    {PMIC_A, A_OCP_CTL, 0x12, ALL, 0},   /* over-current at 350 mA, */
    {PMIC_A, A_OCP_COUNT, 0xff, ALL, 0}, /* 4096 cycles */
    {PMIC_D, SEC_ACCESS, SEC_UNLOCK, ALL, 0},
    {PMIC_D, RESET_CTL3, 0x0f, ALL, 0}, /* the 2.0 defaults */
    {PMIC_A, A_OPAMP_BIAS, 0x4f, ALL, 0},
    {PMIC_A, A_NCP_FBCTRL, 0x28, ALL, 0},
    {PMIC_A, A_SPKR_DRV, 0x69, ALL, 0},
    {PMIC_A, A_SPKR_DBG, 0x01, ALL, 0},
    {PMIC_A, A_BOOST_EN, 0x5f, ALL, 0},
    {PMIC_A, A_SLOPE_COMP, 0x88, ALL, 0},
    {PMIC_A, SEC_ACCESS, SEC_UNLOCK, ALL, 0},
    {PMIC_A, RESET_CTL3, 0x0f, ALL, 0},
    {PMIC_A, A_CURRENT_LIMIT, 0x82, ALL, 0},
    {PMIC_A, A_SPKR_DAC, 0x03, ALL, 0},
    {PMIC_A, A_SPKR_OCP, 0xe1, ALL, 0},
    {PMIC_D, D_CDC_RST, RST_RELEASE, ALL, 0}, /* the digital part running */
};

/* The vendor's power-up of the speaker route, in the order its DAPM runs
 * it: the clock block (msm8x16_wcd_codec_enable_clock_block); the digital
 * codec's supplies; the charge pump, 1 ms to start, and the receive bias;
 * the route (RX1 MIX1 from I2S RX1, RX2 MIX1 from I2S RX2, the right DAC
 * from RX2); each DAC in data reset while its clocks start and it comes on
 * (msm8x16_wcd_hphl_dac_event, hphr); the interpolators, whose 0 dB gains
 * take once written after them, 10 ms on (mainline's
 * msm8916_wcd_digital_enable_interpolator); the receive chains muted; the
 * headphone amplifiers, 7 ms to settle, then each one's test bit and its
 * chain unmuted (msm8x16_wcd_hph_pa_event). */
static const struct step codec_on[] = {
    {DIGITAL, CDC_PDM, PDM_EN | PDM_FB, PDM_EN | PDM_FB, 0},
    {PMIC_A, A_MASTER_BIAS, 0x30, 0x30, 0},
    {PMIC_D, D_CDC_RST, RST_RELEASE, RST_RELEASE, 0},
    {PMIC_D, D_TOP_CLK, A_MCLKS, A_MCLKS, 0},
    {DIGITAL, CDC_OTHR, CDC_CONN, CDC_CONN, 0},
    {DIGITAL, CDC_RX_I2S, RX_I2S_CLK, RX_I2S_CLK, 0},
    {PMIC_D, D_DIG_CLK, NCP_CLK | RXD_PDM_CLK, NCP_CLK | RXD_PDM_CLK, 0},
    {PMIC_A, A_NCP_EN, NCP_ON, NCP_ON, 1000},
    {PMIC_A, A_BIAS_DAC, RX_BIAS, RX_BIAS, 0},
    {PMIC_A, A_BIAS_DAC, DAC_REF, DAC_REF, 0},
    {DIGITAL, CDC_RX1_INPUT, FROM_RX1, INPUT1, 0},
    {DIGITAL, CDC_RX2_INPUT, FROM_RX2, INPUT1, 0},
    {PMIC_D, D_HPHR_DAC, RDAC2_RX2, RDAC2_RX2, 0},
    {PMIC_A, A_HPHL_DAC, DAC_RESET, DAC_RESET, 0},
    {PMIC_D, D_DIG_CLK, RXD1_CLK, RXD1_CLK, 0},
    {PMIC_D, D_ANA_CLK, HPHL_CLK, HPHL_CLK, 0},
    {PMIC_A, A_HPHL_DAC, DAC_ON, DAC_ON, 0},
    {PMIC_A, A_HPHL_DAC, 0, DAC_RESET, 0},
    {PMIC_A, A_HPHR_DAC, DAC_RESET, DAC_RESET, 0},
    {PMIC_D, D_DIG_CLK, RXD2_CLK, RXD2_CLK, 0},
    {PMIC_D, D_ANA_CLK, HPHR_CLK, HPHR_CLK, 0},
    {PMIC_A, A_HPHR_DAC, DAC_ON, DAC_ON, 0},
    {PMIC_A, A_HPHR_DAC, 0, DAC_RESET, 0},
    {DIGITAL, CDC_RX_B1, RX1_RX2, RX1_RX2, 10000},
    {DIGITAL, CDC_RX1_GAIN, 0, ALL, 0},
    {DIGITAL, CDC_RX2_GAIN, 0, ALL, 0},
    {DIGITAL, CDC_RX1_MUTE, RX_MUTE, RX_MUTE, 0},
    {DIGITAL, CDC_RX2_MUTE, RX_MUTE, RX_MUTE, 0},
    {PMIC_A, A_NCP_FBCTRL, FB_CLK_INV, FB_CLK_INV, 0},
    {PMIC_A, A_CNP_EN, HPH_PAS, HPH_PAS, 7000},
    {PMIC_A, A_HPHL_TEST, TEST_ON, TEST_ON, 0},
    {DIGITAL, CDC_RX1_MUTE, 0, RX_MUTE, 0},
    {PMIC_A, A_HPHR_TEST, TEST_ON, TEST_ON, 0},
    {DIGITAL, CDC_RX2_MUTE, 0, RX_MUTE, 0},
};

/* The reverse, once the loudspeaker's amplifier is off: the chains muted,
 * 20 ms, the headphone amplifiers off; the interpolators off and reset, the
 * chains unmuted; the DACs, the route, the bias, the charge pump and the
 * clocks off (the vendor's power-down events, msm8x16_wcd_codec_enable_
 * interpolator, rx_chain and charge_pump among them). The master bias goes
 * off too: the vendor's and mainline's jack detection keep it on, and
 * nothing here senses the jack. */
static const struct step codec_off[] = {
    {DIGITAL, CDC_RX1_MUTE, RX_MUTE, RX_MUTE, 0},
    {DIGITAL, CDC_RX2_MUTE, RX_MUTE, RX_MUTE, 20000},
    {PMIC_A, A_HPHL_TEST, 0, TEST_ON, 0},
    {PMIC_A, A_HPHR_TEST, 0, TEST_ON, 0},
    {PMIC_A, A_CNP_EN, 0, HPH_PAS, 5000},
    {DIGITAL, CDC_RX_B1, 0, RX1_RX2, 0},
    {DIGITAL, CDC_RX_RESET, RX1_RX2, RX1_RX2, 0},
    {DIGITAL, CDC_RX_RESET, 0, RX1_RX2, 0},
    {DIGITAL, CDC_RX1_MUTE, 0, RX_MUTE, 0},
    {DIGITAL, CDC_RX2_MUTE, 0, RX_MUTE, 0},
    {PMIC_A, A_HPHL_DAC, 0, DAC_ON, 0},
    {PMIC_D, D_ANA_CLK, 0, HPHL_CLK, 0},
    {PMIC_D, D_DIG_CLK, 0, RXD1_CLK, 0},
    {PMIC_A, A_HPHR_DAC, 0, DAC_ON, 0},
    {PMIC_D, D_ANA_CLK, 0, HPHR_CLK, 0},
    {PMIC_D, D_DIG_CLK, 0, RXD2_CLK, 0},
    {DIGITAL, CDC_RX1_INPUT, 0, INPUT1, 0},
    {DIGITAL, CDC_RX2_INPUT, 0, INPUT1, 0},
    {PMIC_D, D_HPHR_DAC, 0, RDAC2_RX2, 0},
    {PMIC_A, A_BIAS_DAC, 0, DAC_REF, 0},
    {PMIC_A, A_BIAS_DAC, 0, RX_BIAS, 0},
    {PMIC_A, A_NCP_EN, 0, NCP_ON, 1000},
    {PMIC_D, D_DIG_CLK, 0, NCP_CLK | RXD_PDM_CLK, 0},
    {DIGITAL, CDC_RX_I2S, 0, RX_I2S_CLK, 0},
    {DIGITAL, CDC_OTHR, 0, CDC_CONN, 0},
    {PMIC_D, D_TOP_CLK, 0, A_MCLKS, 0},
    {DIGITAL, CDC_PDM, 0, PDM_EN | PDM_FB, 0},
    {PMIC_A, A_MASTER_BIAS, 0, 0x30, 0},
};

static bool step(const struct step *s) {
    if (s->target == DIGITAL) {
        update(CDC + s->reg, s->value, s->mask);
    } else if (s->mask == ALL) {
        if (!redmi2_power_hw_write(s->target, (uint8_t)s->reg, s->value)) return false;
    } else {
        uint8_t value;
        if (!redmi2_power_hw_read(s->target, (uint8_t)s->reg, &value)) return false;
        uint8_t set = (uint8_t)((value & ~s->mask) | (s->value & s->mask));
        if (set != value && !redmi2_power_hw_write(s->target, (uint8_t)s->reg, set)) return false;
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

/* The loudspeaker's amplifier first (the vendor's Ext Spk, which goes off
 * before the headphone amplifiers), then the rest, whatever failed: each
 * step of codec_off is tried even if one before it failed. */
static void power_down(void) {
    enable(SPEAKER_AMP, false, ENABLE_OFF);
    for (size_t i = 0; i < sizeof(codec_off) / sizeof(codec_off[0]); ++i) (void)step(&codec_off[i]);
    lpaif_stop();
    pdm_pins(false);
    (void)clocks(false);
    state = OFF;
}

/* The ring silent and playing before the codecs and then the loudspeaker's
 * amplifier come up, so that they start on silence. Nothing reaches the
 * audio block's registers unless its clocks run. */
static bool power_up(void) {
    if (!clocks(true)) {
        (void)clocks(false);
        return false;
    }
    state = ON;
    update(SPKR_IOMUX, PRI_WS_SLAVE_SEL_11, PRI_WS_SLAVE_SEL_11);
    pdm_pins(true);
    enable(SPEAKER_AMP, false, ENABLE_ON);
    digital_start();
    played = written = 0;
    cleared = RING;
    read_at = 0;
    moved_at = heard_at = polled_at = time_us();
    if (!lpaif_start() || !RUN(codec_on)) {
        power_down();
        return false;
    }
    enable(SPEAKER_AMP, true, ENABLE_ON);
    return true;
}

/* The DMA's position in the ring: none while it reads outside it. */
static bool position(uint32_t *at) {
    uint32_t current = mmio_read32(RDMA + RDMA_CURR) - (uint32_t)(uintptr_t)ring;
    if (current >= sizeof(ring)) return false;
    *at = current / sizeof(ring[0]);
    return true;
}

static void mix(uint64_t at, uint32_t count) {
    /* Cached: each frame passes through it three times. */
    static struct audio_frame frames[MIX_FRAMES] __attribute__((section(".cpu_private")));
    while (count) {
        uint32_t n = count < MIX_FRAMES ? count : MIX_FRAMES;
        audio_mix_volume(frames, n, volume, true);
        ring_write(at, frames, n);
        at += n;
        count -= n;
    }
}

unsigned redmi2_audio_volume(int step) {
    if (step > 0 && volume < AUDIO_VOLUME_STEPS) ++volume;
    if (step < 0 && volume) --volume;
    return volume;
}

bool redmi2_audio_quiet(void) { return state != ON; }

static uint32_t latency(void) {
    return state == ON && written > played ? (uint32_t)(written - played) : 0;
}

static bool poll(void) {
    if (state == FAILED) return false;
    uint32_t ready = audio_ready();
    if (state == OFF) {
        if (!ready) return true;
        if (!power_up()) {
            state = FAILED;
            return false;
        }
    }
    uint64_t now = time_us();
    uint32_t at;
    /* After a lap or more without a poll, the DMA may stand where it stood,
     * whole laps on: only polls closer than a lap tell a stop. */
    bool seen = position(&at), lapped = now - polled_at >= LAP_US;
    polled_at = now;
    if (seen && (at != read_at || lapped)) {
        /* The position gives the DMA's progress modulo the ring: the laps
         * of a poll later than one are those the time since implies. */
        uint64_t moved = (at + RING - read_at) % RING;
        uint64_t due = (now - moved_at) * USER_AUDIO_RATE / 1000000;
        if (due > moved + RING / 2) moved += (due - moved + RING / 2) / RING * RING;
        played += moved;
        read_at = at;
        moved_at = now;
    } else if (!lapped && now - moved_at >= STALL_US) {
        power_down();
        state = FAILED;
        return false;
    }
    /* Audio from where the last ended, or past the DMA if it ran out. */
    if (ready) {
        uint64_t start = written > played + GUARD ? written : played + GUARD;
        /* After a poll nearly a lap late, frames between the last silence
         * and start still hold the lap before: silence them first. */
        if (cleared < played) cleared = played;
        if (cleared < start) {
            ring_write(cleared, 0, (uint32_t)(start - cleared));
            cleared = start;
        }
        if (start < played + LEAD) {
            uint32_t room = (uint32_t)(played + LEAD - start), n = ready < room ? ready : room;
            mix(start, n);
            written = start + n;
        }
    }
    if (written > played) heard_at = now;
    /* Silence one lap ahead of the DMA, behind the audio. */
    if (cleared < written) cleared = written;
    if (cleared < played) cleared = played;
    if (cleared < played + RING) {
        ring_write(cleared, 0, (uint32_t)(played + RING - cleared));
        cleared = played + RING;
    }
    barrier(); /* the DMA reads what was written */
    if (!ready && written <= played && now - heard_at >= IDLE_US) power_down();
    return true;
}

/* The enables low and the PDM link pulled down, as the vendor's sleep
 * states leave them, then the PM8916's codec reset and set up; the audio
 * block's clocks are gated until a stream plays. */
bool redmi2_audio_init(struct audio_device *out) {
    enable(SPEAKER_AMP, false, ENABLE_OFF);
    enable(HEADPHONE_SWITCH, false, ENABLE_OFF);
    pdm_pins(false);
    (void)clocks(false); /* whatever the codec does, gated for standby */
    uint8_t revision;
    if (!redmi2_power_hw_read(PMIC_D, D_REVISION1, &revision) || !RUN(codec_init)) return false;
    state = OFF;
    *out = (struct audio_device){.poll = poll, .latency = latency};
    return true;
}
