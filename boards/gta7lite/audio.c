#include "audio.h"
#include "gta7lite.h"
#include "platform.h"

/* The tablet's two speakers, played as its own Linux kernel and Android
 * play music through them (Samsung's gta7lite kernel: sound/soc/mediatek's
 * mt6765, common_int, codec/mt6357 and aw87390; the stock vendor partition's
 * AudioParamOptions.xml, whose speaker path is int_hp_buf). MT6765's audio
 * front end (AFE) plays a ring in its own SRAM through its DL1 memory
 * interface and its ADDA downlink, which sends the samples over the MTKAIF
 * pins to the MT6357's DAC; the MT6357's headphone drivers feed two Awinic
 * AW87359 amplifiers on I2C3, one per speaker, set to the stock
 * aw87xxx_acf.bin's Music profile. Headphones in the jack, which the
 * MT6357's ACCDET senses, turn the amplifiers off. Both outputs run at
 * Android's analog levels: the headphone drivers at +2 dB (VolumeGainMap's
 * SPK_HP and HS), the downlink at -0.3 dB; the volume, the speakers' and the
 * headphones' apart, scales the samples. All of it powers up when a stream
 * has frames and down after 3 s with none. The main loop's polls keep the
 * ring filled ahead of the DMA, with silence where the streams have
 * nothing, so that a poll up to a lap late lets the outputs fall silent
 * rather than replay old samples; the laps a later one missed are counted
 * from the time that passed. */

enum {
    /* Clocks, in the order of mt6765's AudDrv_Clk_On (clk-mt6765.c,
     * clk-mt6765-audio.c): infracfg's audio gate; topckgen's audio_sel on
     * clk26m and aud_intbus_sel on syspll1_d4, each with its gate and the
     * update bit that latches it; the AFE's own gates; infracfg's MTKAIF
     * 26 MHz gate. */
    INFRACFG = 0x10001000,
    IFR3_SET = 0x88,
    IFR3_CLR = 0x8c,
    IFR_AUDIO = 1 << 25,
    IFR4_SET = 0xa4,
    IFR4_CLR = 0xa8,
    IFR_AUDIO_26M = 1 << 4,
    TOPCKGEN = 0x10000000,
    CLK_CFG_UPDATE = 0x004,
    CLK_CFG_3 = 0x70,
    AUDIO_SEL = 3 << 24,
    AUDIO_SEL_OFF = 1u << 31,
    AUDIO_SEL_UPDATE = 1 << 15,
    CLK_CFG_4 = 0x80,
    AUD_INTBUS_SEL = 3 << 0,
    AUD_INTBUS_SYSPLL1_D4 = 1 << 0,
    AUD_INTBUS_OFF = 1 << 7,
    AUD_INTBUS_UPDATE = 1 << 16,
    CFG_SET = 4,
    CFG_CLR = 8,
    /* The AFE (mt6765/mtk-auddrv-afe.h, mt6765-sound.c). */
    AFE = 0x11220000,
    AUDIO_TOP_CON0 = 0x000,
    PDN_AFE = 1 << 2,
    PDN_DAC = 1 << 25,
    PDN_DAC_PREDIS = 1 << 26,
    AUDIO_DCM = 1 << 29,
    AFE_DAC_CON0 = 0x010,
    AFE_ON = 1 << 0,
    DL1_ON = 1 << 1,
    AFE_DAC_CON1 = 0x014,
    DL1_MODE = 0xf << 0,
    DL1_48K = 10 << 0,
    DL1_MONO = 1 << 21,
    AFE_CONN3 = 0x02c, /* interconnect: DL1's I05 and I06 to O03 and O04, */
    AFE_CONN4 = 0x030,
    AFE_CONN28 = 0x4bc, /* and to O28 and O29 */
    AFE_CONN29 = 0x4c0,
    I05 = 1 << 5,
    I06 = 1 << 6,
    AFE_I2S_CON1 = 0x034,
    /* SetI2SDacOut: 48 kHz, I2S format, 32-bit words, then on. */
    I2S_DAC_OUT = 10 << 8 | 1 << 3 | 1 << 1,
    I2S_ON = 1 << 0,
    AFE_DL1_BASE = 0x040,
    AFE_DL1_CUR = 0x044,
    AFE_DL1_END = 0x048,
    AFE_CONN_24BIT = 0x06c,
    O03_O04_24BIT = 3 << 3,
    O28_O29_24BIT = 3u << 28,
    AFE_MEMIF_MSB = 0x0cc,
    CPU_HD_8_24 = 1 << 28,
    CPU_SRAM_COMPACT = 1 << 29,
    AFE_ADDA_DL_SRC2_CON0 = 0x108,
    /* SetDLSrc2(48000): 48 kHz [31:28], gain on [25:24], [12:11] and [1]. */
    DL_SRC2_48K = 8u << 28 | 3 << 24 | 3 << 11 | 1 << 1,
    DL_SRC_ON = 1 << 0,
    AFE_ADDA_DL_SRC2_CON1 = 0x10c,
    DL_GAIN_MINUS_0_3_DB = 0xf74f0000u,
    AFE_ADDA_UL_DL_CON0 = 0x124,
    ADDA_ON = 1 << 0,
    AFE_ADDA_PREDIS_CON0 = 0x260,
    AFE_ADDA_PREDIS_CON1 = 0x264,
    AFE_DAC_MON = 0x2ec,
    AFE_MEMIF_HD_MODE = 0x3f8,
    DL1_HD = 3 << 0,
    AFE_MEMIF_HDALIGN = 0x3fc,
    DL1_HD_ALIGN = 1 << 0,
    SRAM_NORMAL = 0x7fff << 16,
    AFE_ADDA_DL_SDM_DCCOMP_CON = 0xc50,
    SDM_LEVEL = 0x3f,
    SDM_LEVEL_NORMAL = 0x1d,
    AFE_OFF_US = 10000,
    /* The ring: the part of the AFE's SRAM that holds 16-bit samples
     * (AFE_INTERNAL_SRAM_NORMAL_SIZE), 144 ms. Writes start 5 ms ahead of
     * the DMA and fill up to 133 ms ahead; positions stay multiples of 4
     * frames (word_size_align). The DMA has stopped when it has not moved
     * for half a second. */
    SRAM = 0x11221000,
    RING = 0x6c00 / sizeof(struct audio_frame),
    GUARD = 256,
    LEAD = RING - 2 * GUARD,
    STALL_US = 500000,
    LAP_US = RING * 1000000ull / USER_AUDIO_RATE, /* 144 ms */
    IDLE_US = 3000000,
    MIX_FRAMES = 512,
    /* The volume: 3 dB a step below full scale, from the speakers' 12
     * (-9 dB) and the headphones' 7 (-24 dB) at each start. */
    SPEAKER_VOLUME = 12,
    HEADPHONE_VOLUME = 7,
    /* The MTKAIF pins, GPIO 136-139 (AUD_CLK_MOSI, AUD_SYNC_MOSI,
     * AUD_DAT_MOSI0 and 1), each 4 bits of one mode register with set and
     * clear registers after it; the stock kernel switches the clock and the
     * data pins to mode 1 while playing (aud_clk_mosi_on, aud_dat_mosi_on)
     * and back to GPIO mode after. */
    GPIO_MODE_136 = 0x10005410,
    GPIO_SET = 4,
    GPIO_CLR = 8,
    MOSI_PINS = 0xf << 0 | 0xf << 8 | 0xf << 12,
    MOSI_MODE = 1 << 0 | 1 << 8 | 1 << 12,
    /* The MT6357's audio registers (mtk-auddrv-ana.h; Linux's
     * sound/soc/codecs/mt6357.h). */
    SMT_CON1 = 0x002c,
    GPIO_DIR0 = 0x0088,
    GPIO_MODE2 = 0x00b6,
    GPIO_MODE2_SET = 0x00b8,
    GPIO_MODE2_CLR = 0x00ba,
    GPIO_MODE3 = 0x00bc,
    GPIO_MODE3_CLR = 0x00c0,
    DCXO_CW14 = 0x07ac,
    XO_AUDIO_EN = 1 << 13,
    AUD_TOP_CKPDN_CON0 = 0x208c,
    AUDNCP_CLKDIV_CON0 = 0x20b4,
    AUDNCP_CLKDIV_CON1 = 0x20b6,
    AUDNCP_CLKDIV_CON2 = 0x20b8,
    AUDNCP_CLKDIV_CON3 = 0x20ba,
    AUDNCP_CLKDIV_CON4 = 0x20bc,
    AFE_UL_DL_CON0 = 0x2108,
    AFE_DL_SRC2_CON0_L = 0x210a,
    PMIC_AFE_TOP_CON0 = 0x2110,
    PMIC_AUDIO_TOP_CON0 = 0x2112,
    AFUNC_AUD_CON0 = 0x2116,
    AFUNC_AUD_CON2 = 0x211a,
    AFE_AUD_PAD_TOP = 0x214c,
    AUDENC_ANA_CON6 = 0x2194,
    AUDENC_ANA_CON10 = 0x219c,
    AUDDEC_ANA_CON0 = 0x2208,
    AUDDEC_ANA_CON1 = 0x220a,
    AUDDEC_ANA_CON2 = 0x220c,
    AUDDEC_ANA_CON3 = 0x220e,
    AUDDEC_ANA_CON4 = 0x2210,
    AUDDEC_ANA_CON6 = 0x2214,
    AUDDEC_ANA_CON7 = 0x2216,
    AUDDEC_ANA_CON8 = 0x2218,
    AUDDEC_ANA_CON9 = 0x221a,
    AUDDEC_ANA_CON10 = 0x221c,
    AUDDEC_ANA_CON11 = 0x221e,
    AUDDEC_ANA_CON12 = 0x2220,
    AUDDEC_ANA_CON13 = 0x2222,
    ZCD_CON0 = 0x2288,
    ZCD_CON2 = 0x228c,
    /* ACCDET (Samsung's sound/soc/codecs/mt6357-accdet.c and mt6357.h): its
     * clock (AUD_TOP_CKPDN_CON0 bit 0, on from reset) and reset, then EINT0,
     * the comparator on the jack's detect pin, sampled by its PWM as
     * config_eint_init_by_mode sets it up. Its input is low while a plug is
     * in (the stock DT's headset-eint-level-pol, level low); the comparator's
     * own debounce is 256 ms (ACCDET_CON15 from reset), and a change counts
     * once two samples 100 ms apart agree. */
    AUD_TOP_RST_CON0 = 0x209a,
    ACCDET_RST = 1 << 1,
    ACCDET_CON1 = 0x230a,
    ACCDET_SEQ_INIT = 1 << 1,
    EINT0_EN = 1 << 2,
    ACCDET_CON2 = 0x230c,
    EINT0_PWM = 1 << 3 | 1 << 11, /* EINT0_PWM_EN, EINT0_PWM_IDLE */
    ACCDET_CON15 = 0x2326,
    EINT0_PWM_TIMING = 6 << 8 | 2 << 12, /* PWM_THRESH and PWM_WIDTH bits */
    ACCDET_CON18 = 0x232c,
    EINT0_CUR_IN = 1 << 8,
    JACK_SAMPLE_US = 100000,
    ALL = 0xffff,
    /* Headphone gain steps, 1 dB each from +8 dB (index 0) to -12 dB
     * (index 20), and -40 dB (0x1f), for left [4:0] and right [11:7]. */
    HP_GAIN = 0x0f9f,
    HP_GAIN_MUTE = 0x1f,
    HP_GAIN_QUIETEST = 20,
    HP_GAIN_SPEAKER = 6,
    /* The AW87359s (Samsung's aw87xxx: chip ID 0x59 without a reset pin is
     * its pid_59_3x9 class). */
    AW87359 = 0x59,
    AW_CHIPID = 0x00,
    AW_SYSCTRL = 0x01
};

struct step {
    uint16_t address, value, mask, wait_us;
};

/* mt6357_codec_init_reg: the codec as the stock kernel leaves it at boot,
 * powered down and its MOSI and MISO pins in GPIO mode. */
static const struct step codec_rest[] = {
    {DCXO_CW14, XO_AUDIO_EN, XO_AUDIO_EN, 0},
    {AUDENC_ANA_CON6, 0x0000, 0x0001, 0},
    {AUDDEC_ANA_CON11, 0x0010, 0x0010, 0},
    {AUD_TOP_CKPDN_CON0, 0x0066, 0x0066, 0},
    {PMIC_AUDIO_TOP_CON0, 0x00ff, 0x00ff, 0},
    {AUDDEC_ANA_CON0, 0x3000, 0x3000, 0},
    {AUDDEC_ANA_CON3, 0x0010, 0x0010, 0},
    {AUDDEC_ANA_CON4, 0x0010, 0x0010, 0},
    {AUDENC_ANA_CON10, 0x1c07, ALL, 0},
    {GPIO_MODE2_CLR, 0xffff, ALL, 0},
    {GPIO_MODE2, 0x0000, ALL, 0},
    {GPIO_DIR0, 0x0000, 0x0f00, 0},
    {SMT_CON1, 0x0000, 0x0ff0, 0},
    {GPIO_MODE3_CLR, 0xffff, ALL, 0},
    {GPIO_MODE3, 0x0000, ALL, 0},
    {GPIO_DIR0, 0x0000, 0xf000, 0},
    {SMT_CON1, 0x0000, 0x0ff0, 0},
    {DCXO_CW14, 0, XO_AUDIO_EN, 0},
};

/* TurnOnDacPower for the headphone outputs, then Audio_Amp_Change's start up
 * to its output stages' ramps. */
static const struct step codec_on[] = {
    {DCXO_CW14, XO_AUDIO_EN, XO_AUDIO_EN, 0},
    {GPIO_MODE2_CLR, 0xffff, ALL, 0},
    {GPIO_MODE2_SET, 0x0249, ALL, 0},
    {GPIO_MODE2, 0x0249, ALL, 0},
    {SMT_CON1, 0x0ff0, 0x0ff0, 0},
    {AUDDEC_ANA_CON6, 0x0280, 0x0fff, 0},
    {AUDDEC_ANA_CON7, 0x00a8, ALL, 0},
    {AUDDEC_ANA_CON6, 0x0000, 0x0fff, 0},
    {AUDDEC_ANA_CON7, 0x00a0, 0x0fff, 0},
    {AUDDEC_ANA_CON11, 0x0000, 0x0010, 0}, /* AUDGLB */
    {AUDDEC_ANA_CON2, 0x0400, 0x0400, 0},  /* pull-down HPL/R */
    {AUDENC_ANA_CON6, 0x0001, 0x0001, 0},  /* CLKSQ */
    {AUD_TOP_CKPDN_CON0, 0x0000, 0x0066, 250},
    {PMIC_AUDIO_TOP_CON0, 0x0000, 0x00c4, 250},
    {AFUNC_AUD_CON2, 0x0006, ALL, 0},
    {AFUNC_AUD_CON0, 0xcba1, ALL, 0},
    {AFUNC_AUD_CON2, 0x0003, ALL, 0},
    {AFUNC_AUD_CON2, 0x000b, ALL, 0},
    {AFE_UL_DL_CON0, 0x0001, 0x0001, 0},
    {AFE_DL_SRC2_CON0_L, 0x0001, ALL, 0},
    {PMIC_AFE_TOP_CON0, 0x0000, ALL, 0},
    {AUDDEC_ANA_CON0, 0x3000, ALL, 0},
    {AUDDEC_ANA_CON3, 0x0010, ALL, 0},
    {AUDDEC_ANA_CON4, 0x0010, ALL, 0},
    {AUDDEC_ANA_CON2, 0x0200, 0x0200, 0},
    {ZCD_CON2, HP_GAIN_MUTE << 7 | HP_GAIN_MUTE, ALL, 0},
    {AUDNCP_CLKDIV_CON1, 0x0001, ALL, 0},
    {AUDNCP_CLKDIV_CON2, 0x002c, ALL, 0},
    {AUDNCP_CLKDIV_CON0, 0x0001, ALL, 0},
    {AUDNCP_CLKDIV_CON4, 0x0002, ALL, 0},
    {AUDNCP_CLKDIV_CON3, 0x0000, ALL, 250},
    {AUDDEC_ANA_CON12, 0x1055, 0x1055, 0},
    {AUDDEC_ANA_CON13, 0x0001, ALL, 100},
    {AUDDEC_ANA_CON8, 0x0001, 0x0007, 0}, /* zero-crossing detection */
    {ZCD_CON0, 0x010b, 0x07ff, 0},
    {AUDDEC_ANA_CON10, 0x0055, ALL, 0},
    {AUDDEC_ANA_CON9, 0x0092, ALL, 0},
    {AUDDEC_ANA_CON2, 0x0033, 0x00ff, 0},
    {AUDDEC_ANA_CON1, 0x000c, ALL, 0},
    {AUDDEC_ANA_CON1, 0x003c, ALL, 0},
    {AUDDEC_ANA_CON6, 0x0c80, 0x0fff, 0},
    {AUDDEC_ANA_CON0, 0x30c0, ALL, 0},
    {AUDDEC_ANA_CON0, 0x30f0, ALL, 0},
    {AUDDEC_ANA_CON1, 0x00fc, 0x00ff, 0},
    {AUDDEC_ANA_CON6, 0x0e80, 0x0fff, 0},
    {AUDDEC_ANA_CON6, 0x0280, 0x0fff, 0},
    {AUDDEC_ANA_CON1, 0x00ff, ALL, 0},
};

/* After the ramps and the gain: the DAC on and switched to the headphone
 * drivers, then their pull-down released. */
static const struct step codec_play[] = {
    {AUDDEC_ANA_CON1, 0x77c3, ALL, 0},      /* aux output stage off */
    {AUDDEC_ANA_CON1, 0x7703, ALL, 100},    /* main output unshorted */
    {AUDDEC_ANA_CON11, 0x0001, 0x0001, 0},  /* AUD_CLK */
    {AUDDEC_ANA_CON0, 0x30ff, ALL, 0},      /* the DAC */
    {AUDDEC_ANA_CON6, 0x0281, 0x0fff, 100}, /* its low-noise mode */
    {AUDDEC_ANA_CON0, 0x32ff, ALL, 0},      /* HPL, then HPR, on the DAC */
    {AUDDEC_ANA_CON0, 0x3aff, ALL, 0},
    {AUDDEC_ANA_CON2, 0x0000, 0x0400, 0}, /* pull-down released */
};

/* Audio_Amp_Change's stop, before its gain and ramps. */
static const struct step codec_mute[] = {
    {AUDDEC_ANA_CON2, 0x0400, 0x0400, 0},  /* pull-down */
    {AUDDEC_ANA_CON0, 0x0000, 0x0f00, 0},  /* HPL and HPR off the DAC */
    {AUDDEC_ANA_CON6, 0x0000, 0x0001, 0},  /* the DAC's low-noise mode */
    {AUDDEC_ANA_CON0, 0x0000, 0x000f, 0},  /* the DAC */
    {AUDDEC_ANA_CON11, 0x0000, 0x0001, 0}, /* AUD_CLK */
    {AUDDEC_ANA_CON1, 0x77c3, ALL, 0},     /* main output shorted to aux */
    {AUDDEC_ANA_CON1, 0x77cf, ALL, 0},     /* aux output stage on */
};

/* The rest of the stop after the ramps, then TurnOffDacPower. */
static const struct step codec_off[] = {
    {AUDDEC_ANA_CON1, 0x0000, 0x0003, 0},
    {AUDDEC_ANA_CON6, 0x0e00, 0x0fff, 0},
    {AUDDEC_ANA_CON6, 0x0c00, 0x0fff, 0},
    {AUDDEC_ANA_CON1, 0x0000, 0x00c0, 0},
    {AUDDEC_ANA_CON0, 0x0000, 0x0030, 0},
    {AUDDEC_ANA_CON0, 0x0000, 0x00c0, 0},
    {AUDDEC_ANA_CON6, 0x0200, ALL, 0},
    {AUDDEC_ANA_CON1, 0x0000, 0x0030, 0},
    {AUDDEC_ANA_CON1, 0x0000, 0x000c, 0},
    {AUDDEC_ANA_CON7, 0x00a8, 0x00ff, 0},
    {AUDDEC_ANA_CON10, 0x0100, 0x0100, 0},
    {AUDDEC_ANA_CON8, 0x0004, 0x0007, 0},
    {ZCD_CON0, 0x0000, ALL, 0},
    {AUDDEC_ANA_CON13, 0x0000, 0x0001, 0},
    {AUDDEC_ANA_CON12, 0x0000, 0x1055, 0},
    {AUDNCP_CLKDIV_CON3, 0x0001, 0x0001, 0},
    {AFE_DL_SRC2_CON0_L, 0x0000, ALL, 0},
    {AFUNC_AUD_CON2, 0x0000, ALL, 0},
    {AFUNC_AUD_CON0, 0xcba0, ALL, 0},
    {AFE_UL_DL_CON0, 0x0000, 0x0001, 0},
    {PMIC_AUDIO_TOP_CON0, 0x00df, 0x00df, 0},
    {AFE_AUD_PAD_TOP, 0x0000, 0x00ff, 250},
    {GPIO_MODE2_CLR, 0xffff, ALL, 0},
    {GPIO_MODE2, 0x0000, ALL, 0},
    {GPIO_DIR0, 0x0000, 0x0f00, 0},
    {SMT_CON1, 0x0000, 0x0ff0, 0},
    {AUD_TOP_CKPDN_CON0, 0x0066, 0x0066, 0},
    {AUDENC_ANA_CON6, 0x0000, 0x0001, 0},
    {AUDDEC_ANA_CON2, 0x0000, 0x0400, 0},
    {AUDDEC_ANA_CON11, 0x0010, 0x0010, 0},
    {DCXO_CW14, 0, XO_AUDIO_EN, 0},
};

/* The amplifiers' Music profile, register and value, per amplifier: the
 * stock aw87xxx_acf.bin's for aw87359 dev0 (I2C address 0x58) and dev1
 * (0x59), which differ only in PAGC3OPR (0x06). It ends with SYSCTRL's
 * enable; Off is SYSCTRL cleared, twice. */
static const uint8_t amp_music[2][44] = {
    {0x70, 0x80, 0x01, 0x00, 0x01, 0x00, 0x02, 0x0c, 0x03, 0x08, 0x04, 0x05, 0x05, 0x0c, 0x06,
     0x0a, 0x07, 0xae, 0x08, 0x09, 0x09, 0x08, 0x0a, 0x4b, 0x61, 0xbb, 0x62, 0x80, 0x63, 0x29,
     0x64, 0x58, 0x65, 0xcd, 0x66, 0x3c, 0x67, 0x2f, 0x68, 0x07, 0x69, 0xdb, 0x01, 0x0d},
    {0x70, 0x80, 0x01, 0x00, 0x01, 0x00, 0x02, 0x0c, 0x03, 0x08, 0x04, 0x05, 0x05, 0x0c, 0x06,
     0x09, 0x07, 0xae, 0x08, 0x09, 0x09, 0x08, 0x0a, 0x4b, 0x61, 0xbb, 0x62, 0x80, 0x63, 0x29,
     0x64, 0x58, 0x65, 0xcd, 0x66, 0x3c, 0x67, 0x2f, 0x68, 0x07, 0x69, 0xdb, 0x01, 0x0d}};
static const uint8_t amp_address[2] = {0x58, 0x59};

static enum { OFF, ON, FAILED } state;

static unsigned volume[2] = {SPEAKER_VOLUME, HEADPHONE_VOLUME};

/* The jack: whether ACCDET is sensing it, its last sample, the state taken. */
static struct {
    bool ready, sample, plugged;
    uint64_t sampled;
} jack;

/* Frame counts since power-up: played by the DMA, as its position moved;
 * audio written; and the ring written up to, audio or silence. read_at is
 * the DMA's position in the ring at the last poll. */
static uint64_t played, written, cleared;
static uint32_t read_at;
static uint64_t moved_at, heard_at, polled_at;

static void afe(unsigned offset, uint32_t value, uint32_t mask) {
    uintptr_t at = AFE + offset;
    mmio_write32(at, (mmio_read32(at) & ~mask) | (value & mask));
}

/* A topckgen mux to a parent, latched only when it changes, as Linux's mux
 * operations do; then its gate on or off. */
static void mux(unsigned config, uint32_t select, uint32_t parent, uint32_t gate, uint32_t update,
                bool on) {
    uint32_t value = mmio_read32(TOPCKGEN + config);
    if ((value & select) != parent) {
        mmio_write32(TOPCKGEN + config + CFG_CLR, select);
        mmio_write32(TOPCKGEN + config + CFG_SET, parent);
        mmio_write32(TOPCKGEN + CLK_CFG_UPDATE, update);
    }
    mmio_write32(TOPCKGEN + config + (on ? CFG_CLR : CFG_SET), gate);
}

static void clocks(bool on) {
    if (on) {
        mmio_write32(INFRACFG + IFR3_CLR, IFR_AUDIO);
        mux(CLK_CFG_3, AUDIO_SEL, 0, AUDIO_SEL_OFF, AUDIO_SEL_UPDATE, true);
        mux(CLK_CFG_4, AUD_INTBUS_SEL, AUD_INTBUS_SYSPLL1_D4, AUD_INTBUS_OFF, AUD_INTBUS_UPDATE,
            true);
        afe(AUDIO_TOP_CON0, 0, PDN_AFE | PDN_DAC | PDN_DAC_PREDIS);
        mmio_write32(INFRACFG + IFR4_CLR, IFR_AUDIO_26M);
        afe(AUDIO_TOP_CON0, AUDIO_DCM, AUDIO_DCM);
        return;
    }
    mmio_write32(INFRACFG + IFR4_SET, IFR_AUDIO_26M);
    afe(AUDIO_TOP_CON0, PDN_AFE | PDN_DAC | PDN_DAC_PREDIS, PDN_AFE | PDN_DAC | PDN_DAC_PREDIS);
    mux(CLK_CFG_4, AUD_INTBUS_SEL, 0, AUD_INTBUS_OFF, AUD_INTBUS_UPDATE, false);
    mux(CLK_CFG_3, AUDIO_SEL, 0, AUDIO_SEL_OFF, AUDIO_SEL_UPDATE, false);
    mmio_write32(INFRACFG + IFR3_SET, IFR_AUDIO);
}

static void mosi_pins(bool on) {
    mmio_write32(GPIO_MODE_136 + GPIO_CLR, MOSI_PINS);
    if (on) mmio_write32(GPIO_MODE_136 + GPIO_SET, MOSI_MODE);
}

static void ring_write(uint64_t at, const struct audio_frame *frames, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i, ++at) {
        uint32_t word =
            frames ? (uint16_t)frames[i].left | (uint32_t)(uint16_t)frames[i].right << 16 : 0;
        mmio_write32(SRAM + (uint32_t)(at % RING) * sizeof(struct audio_frame), word);
    }
}

/* mtk_pcm_dl1_params, mtk_pcm_prepare and mtk_pcm_dl1_start for 16-bit
 * stereo at 48 kHz from SRAM: the ring silent, the downlink and DL1 on. */
static void afe_start(void) {
    ring_write(0, 0, RING);
    afe(AFE_MEMIF_HDALIGN, SRAM_NORMAL, SRAM_NORMAL | DL1_HD_ALIGN);
    afe(AFE_MEMIF_MSB, 0, CPU_SRAM_COMPACT | CPU_HD_8_24);
    afe(AFE_MEMIF_HD_MODE, 0, DL1_HD);
    mmio_write32(AFE + AFE_DL1_BASE, SRAM);
    mmio_write32(AFE + AFE_DL1_END, SRAM + RING * sizeof(struct audio_frame) - 1);
    afe(AFE_CONN_24BIT, 0, O03_O04_24BIT | O28_O29_24BIT);
    mmio_write32(AFE + AFE_ADDA_PREDIS_CON0, 0);
    mmio_write32(AFE + AFE_ADDA_PREDIS_CON1, 0);
    mmio_write32(AFE + AFE_ADDA_DL_SRC2_CON0, DL_SRC2_48K);
    mmio_write32(AFE + AFE_ADDA_DL_SRC2_CON1, DL_GAIN_MINUS_0_3_DB);
    afe(AFE_ADDA_DL_SDM_DCCOMP_CON, SDM_LEVEL_NORMAL, SDM_LEVEL);
    mmio_write32(AFE + AFE_I2S_CON1, I2S_DAC_OUT);
    afe(AFE_ADDA_UL_DL_CON0, ADDA_ON, ADDA_ON);
    afe(AFE_ADDA_DL_SRC2_CON0, DL_SRC_ON, DL_SRC_ON);
    afe(AFE_I2S_CON1, I2S_ON, I2S_ON);
    mosi_pins(true);
    afe(AFE_CONN3, I05, I05);
    afe(AFE_CONN4, I06, I06);
    afe(AFE_CONN28, I05, I05);
    afe(AFE_CONN29, I06, I06);
    afe(AFE_DAC_CON1, DL1_48K, DL1_MODE | DL1_MONO);
    afe(AFE_DAC_CON0, DL1_ON, DL1_ON);
    afe(AFE_DAC_CON0, AFE_ON, AFE_ON);
}

/* mtk_pcm_dl1_stop and mtk_soc_pcm_dl1_close: DL1 and the downlink off,
 * then the AFE once its monitor says it stopped. */
static void afe_stop(void) {
    afe(AFE_DAC_CON0, 0, DL1_ON);
    afe(AFE_CONN3, 0, I05);
    afe(AFE_CONN4, 0, I06);
    afe(AFE_CONN28, 0, I05);
    afe(AFE_CONN29, 0, I06);
    afe(AFE_ADDA_DL_SRC2_CON0, 0, DL_SRC_ON);
    afe(AFE_I2S_CON1, 0, I2S_ON);
    afe(AFE_ADDA_UL_DL_CON0, 0, ADDA_ON);
    delay_us(125);
    mosi_pins(false);
    afe(AFE_DAC_CON0, 0, AFE_ON);
    for (uint64_t start = time_us();
         (mmio_read32(AFE + AFE_DAC_MON) & AFE_ON) && time_us() - start < AFE_OFF_US;) { }
}

static bool run(const struct step *steps, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!gta7lite_pmic_update(steps[i].address, steps[i].value, steps[i].mask)) return false;
        if (steps[i].wait_us) delay_us(steps[i].wait_us);
    }
    return true;
}

#define RUN(steps) run(steps, sizeof(steps) / sizeof(steps[0]))

/* hp_main_output_ramp and hp_aux_feedback_loop_gain_ramp, 600 us a step. */
static bool output_ramp(bool up) {
    for (unsigned i = 0; i <= 7; ++i) {
        unsigned stage = up ? i : 7 - i;
        if (!gta7lite_pmic_update(AUDDEC_ANA_CON1, (uint16_t)(stage << 8 | stage << 12), 0x7700))
            return false;
        delay_us(600);
    }
    return true;
}

static bool feedback_ramp(bool up) {
    for (unsigned i = 0; i <= 15; ++i) {
        unsigned stage = up ? i : 15 - i;
        if (!gta7lite_pmic_update(AUDDEC_ANA_CON6, (uint16_t)(stage << 12), 0xf000)) return false;
        delay_us(600);
    }
    return true;
}

/* headset_volume_ramp from -40 dB to the speakers' gain through the steps
 * from -12 dB, or back up those steps to -40 dB, 250 us each. */
static bool gain_ramp(bool up) {
    for (unsigned i = 0; i <= HP_GAIN_QUIETEST - HP_GAIN_SPEAKER; ++i) {
        unsigned index = up ? HP_GAIN_QUIETEST - i : HP_GAIN_SPEAKER + 1 + i;
        if (index > HP_GAIN_QUIETEST) index = HP_GAIN_MUTE;
        if (!gta7lite_pmic_update(ZCD_CON2, (uint16_t)(index << 7 | index), HP_GAIN)) return false;
        delay_us(250);
    }
    return true;
}

static bool codec(bool on) {
    if (on) {
        if (!RUN(codec_on) || !output_ramp(true)) return false;
        delay_us(1000);
        return feedback_ramp(true) && gta7lite_pmic_update(AUDDEC_ANA_CON1, 0x77cf, ALL) &&
               gain_ramp(true) && RUN(codec_play);
    }
    return RUN(codec_mute) && gain_ramp(false) &&
           gta7lite_pmic_update(AUDDEC_ANA_CON1, 0x77ff, ALL) && feedback_ramp(false) &&
           output_ramp(false) && RUN(codec_off);
}

static bool amp_write(unsigned amp, uint8_t reg, uint8_t value) {
    return gta7lite_i2c_transfer(GTA7LITE_I2C3, amp_address[amp], reg, true, &value);
}

/* Ext_Speaker_Amp_Change: the Music profile after 1 ms and 5 ms to settle,
 * or Off and 500 us. */
static bool amps(bool on) {
    bool done = true;
    if (on) delay_us(1000);
    for (unsigned amp = 0; amp < 2; ++amp) {
        if (on) {
            for (unsigned i = 0; i < sizeof(amp_music[amp]); i += 2)
                if (!amp_write(amp, amp_music[amp][i], amp_music[amp][i + 1])) return false;
        } else {
            done = amp_write(amp, AW_SYSCTRL, 0) && amp_write(amp, AW_SYSCTRL, 0) && done;
        }
    }
    delay_us(on ? 5000 : 500);
    return done;
}

static void power_down(void) {
    (void)amps(false);
    (void)codec(false);
    afe_stop();
    clocks(false);
    state = OFF;
}

/* The ring silent and playing before the codec and then the amplifiers
 * come up, so that they start on silence. */
static bool power_up(void) {
    clocks(true);
    afe_start();
    played = written = 0;
    cleared = RING;
    read_at = 0;
    moved_at = heard_at = polled_at = time_us();
    state = ON;
    if (codec(true) && (jack.plugged || amps(true))) return true;
    power_down();
    return false;
}

/* The DMA's position in the ring: none while it reads outside it. */
static bool position(uint32_t *at) {
    uint32_t current = mmio_read32(AFE + AFE_DL1_CUR) - SRAM;
    if (current >= RING * sizeof(struct audio_frame)) return false;
    *at = current / sizeof(struct audio_frame);
    return true;
}

static void mix(uint64_t at, uint32_t count) {
    /* Cached: each frame passes through it three times. */
    static struct audio_frame frames[MIX_FRAMES] __attribute__((section(".cpu_private")));
    while (count) {
        uint32_t n = count < MIX_FRAMES ? count : MIX_FRAMES;
        audio_mix_volume(frames, n, volume[jack.plugged], false);
        ring_write(at, frames, n);
        at += n;
        count -= n;
    }
}

/* A plug in or out, once two samples agree: the amplifiers follow it while
 * the outputs are up. */
static void jack_poll(void) {
    uint64_t now = time_us();
    uint16_t value;
    if (!jack.ready || now - jack.sampled < JACK_SAMPLE_US) return;
    jack.sampled = now;
    if (!gta7lite_pmic_read(ACCDET_CON18, &value)) return;
    bool plugged = !(value & EINT0_CUR_IN);
    if (plugged != jack.sample) {
        jack.sample = plugged;
        return;
    }
    if (plugged == jack.plugged) return;
    jack.plugged = plugged;
    if (state == ON) (void)amps(!plugged);
}

/* ACCDET reset, its sequence initialized (accdet_init), then EINT0 sensing
 * (config_eint_init_by_mode); the jack's state from its first samples. */
static bool jack_init(void) {
    if (!gta7lite_pmic_update(AUD_TOP_RST_CON0, ACCDET_RST, ACCDET_RST) ||
        !gta7lite_pmic_update(AUD_TOP_RST_CON0, 0, ACCDET_RST) ||
        !gta7lite_pmic_update(ACCDET_CON1, ACCDET_SEQ_INIT, ACCDET_SEQ_INIT))
        return false;
    delay_us(2000);
    if (!gta7lite_pmic_update(ACCDET_CON1, 0, ACCDET_SEQ_INIT)) return false;
    delay_us(1000);
    return gta7lite_pmic_update(ACCDET_CON15, EINT0_PWM_TIMING, EINT0_PWM_TIMING) &&
           gta7lite_pmic_update(ACCDET_CON2, EINT0_PWM, EINT0_PWM) &&
           gta7lite_pmic_update(ACCDET_CON1, EINT0_EN, EINT0_EN);
}

unsigned gta7lite_audio_volume(int step) {
    unsigned *level = &volume[jack.plugged];
    if (step > 0 && *level < AUDIO_VOLUME_STEPS) ++*level;
    if (step < 0 && *level) --*level;
    return *level;
}

bool gta7lite_audio_headphones(void) { return jack.plugged; }

static uint32_t latency(void) {
    return state == ON && written > played ? (uint32_t)(written - played) : 0;
}

static bool poll(void) {
    if (state == FAILED) return false;
    jack_poll();
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
        uint64_t start = written > played + GUARD ? written : (played + GUARD + 3) & ~3ull;
        /* After a poll nearly a lap late, frames between the last silence
         * and start still hold the lap before: silence them first. */
        if (cleared < played) cleared = played;
        if (cleared < start) {
            ring_write(cleared, 0, (uint32_t)(start - cleared));
            cleared = start;
        }
        if (start < played + LEAD) {
            uint32_t room = (uint32_t)(played + LEAD - start) & ~3u;
            uint32_t n = (ready + 3) & ~3u;
            if (n > room) n = room;
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
    if (!ready && written <= played && now - heard_at >= IDLE_US) power_down();
    return true;
}

bool gta7lite_audio_init(struct audio_device *out) {
    if (!gta7lite_pmic_init()) return false;
    gta7lite_i2c3_init();
    for (unsigned amp = 0; amp < 2; ++amp) {
        uint8_t id;
        if (!gta7lite_i2c_transfer(GTA7LITE_I2C3, amp_address[amp], AW_CHIPID, false, &id) ||
            id != AW87359)
            return false;
    }
    if (!RUN(codec_rest) || !amps(false)) return false;
    clocks(false);
    jack.ready = jack_init();
    state = OFF;
    *out = (struct audio_device){.poll = poll, .latency = latency};
    return true;
}
