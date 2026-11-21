#include "nexus5.h"
#include "platform.h"
#include "qup_i2c.h"

/* The Nexus 5's I2C buses: QUPs of BLSP1 (0xF9923000-0xF9928000) and BLSP2
 * (0xF9963000-0xF9968000), as Linux's gcc-msm8974.c and pinctrl-msm8x74.c
 * name their clocks and pins. A bus needs its BLSP's AHB clock, voted in
 * APCS_CLOCK_BRANCH_ENA_VOTE (bit 17 BLSP1's, 15 BLSP2's), its QUP's I2C
 * core clock, whose source must be the 19.2 MHz XO undivided (qup_i2c.c
 * divides that), and its two pins in their I2C function (3), at 2 mA
 * without pulls: the board's resistors pull the lines up. */

enum {
    GCC = 0xfc400000,
    CLOCK_VOTE = GCC + 0x1484,
    BLSP1_AHB_CBCR = GCC + 0x05c4,
    BLSP2_AHB_CBCR = GCC + 0x0944,
    /* QUP n's I2C core clock branch: BLSP1's at 0x0648 + 0x80 n, BLSP2's at
     * 0x0988 + 0x80 n; its RCG's command and configuration registers 0x18
     * and 0x1c after it. */
    BLSP1_QUP_CBCR = GCC + 0x0648,
    BLSP2_QUP_CBCR = GCC + 0x0988,
    RCG_CMD = 0x18,
    RCG_CFG = 0x1c,
    RCG_SOURCE_DIVIDER = 0x71f, /* the source (10:8, 0 the XO) and divider (4:0) */
    RCG_UPDATE = 1,
    BRANCH_ENABLE = 1,
    CLOCK_OFF = 1u << 31,
    BLSP1_QUP = 0xf9923000,
    BLSP2_QUP = 0xf9963000,
    I2C_FUNCTION = 3, /* shifted by 2; drive 2 mA and no pull are 0 */
    WAIT_US = 1000,
};

bool nexus5_i2c_start(struct qup_i2c *bus, unsigned sda, unsigned scl) {
    bool second = bus->base >= BLSP2_QUP;
    uintptr_t first = second ? BLSP2_QUP : BLSP1_QUP;
    if (bus->base < BLSP1_QUP || bus->base - first >= 6 * 0x1000 || (bus->base & 0xfff))
        return false;
    unsigned qup = (unsigned)((bus->base - first) / 0x1000);
    uintptr_t ahb = second ? BLSP2_AHB_CBCR : BLSP1_AHB_CBCR,
              core = (second ? BLSP2_QUP_CBCR : BLSP1_QUP_CBCR) + 0x80u * qup;
    if (mmio_read32(core + RCG_CFG) & RCG_SOURCE_DIVIDER & ~1u) {
        /* Not the XO undivided: switch it there. */
        mmio_write32(core + RCG_CFG, mmio_read32(core + RCG_CFG) & ~RCG_SOURCE_DIVIDER);
        mmio_write32(core + RCG_CMD, mmio_read32(core + RCG_CMD) | RCG_UPDATE);
        barrier();
        if (!nexus5_wait_bits(core + RCG_CMD, RCG_UPDATE, false, WAIT_US)) return false;
    }
    mmio_write32(CLOCK_VOTE, mmio_read32(CLOCK_VOTE) | (second ? 1u << 15 : 1u << 17));
    mmio_write32(core, mmio_read32(core) | BRANCH_ENABLE);
    barrier();
    if (!nexus5_wait_bits(ahb, CLOCK_OFF, false, WAIT_US) ||
        !nexus5_wait_bits(core, CLOCK_OFF, false, WAIT_US))
        return false;
    mmio_write32(nexus5_gpio(sda), I2C_FUNCTION << 2);
    mmio_write32(nexus5_gpio(scl), I2C_FUNCTION << 2);
    barrier();
    return qup_i2c_init(bus);
}
