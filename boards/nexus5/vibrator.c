#include "nexus5.h"
#include "platform.h"

/* The vibrator, as Google's hammerhead device tree and its stock driver
 * (drivers/misc/msm8974_pwm_vibrator.c) drive it: a motor driver enabled by
 * GPIO60 and steered by a PWM on GPIO27 (its gp1_clk function), MMCC's
 * CAMSS GP1 clock run as an M/N:D counter from the 19.2 MHz XO divided by 4
 * over a period of 127, about 37.8 kHz. Its duty is the drive: half is
 * none, more turns the motor forward, less brakes it. Its supply, PM8941's
 * L19 at 3.3 V, feeds nothing else and the bootloader leaves it off; the
 * stock kernel asks the RPM for it, here it is switched on directly. A
 * pulse runs at the stock driver's strongest forward duty (amplitude 100:
 * 126 of 127) and ends with its brake (amplitude -100: 2 of 127) for 15 ms. */
enum {
    MMCC = 0xfd8c0000,
    GP1_CMD = MMCC + 0x3450,
    GP1_CFG = MMCC + 0x3454,
    GP1_M = MMCC + 0x3458,
    GP1_N = MMCC + 0x345c,
    GP1_D = MMCC + 0x3460,
    GP1_CBCR = MMCC + 0x3474,
    RCG_UPDATE = 1,
    BRANCH_ENABLE = 1,
    CLOCK_OFF = 1u << 31,
    DUAL_EDGE_XO_BY_4 = 2 << 12 | 0 << 8 | 7, /* M/N:D mode, the XO, divider 4 */
    PERIOD = 127,
    FORWARD = 126,
    BRAKE = 2,
    BRAKE_US = 15000,
    WAIT_US = 500,
    PWM_PIN = 27,
    ENABLE_PIN = 60,
    GP1_FUNCTION = 6,
    PIN_OUTPUT = 1 << 9,
    PIN_HIGH = 2,
    /* PM8941 L19 (slave 1, 0x5200; qpnp-regulator.c): status, enable. */
    L19_STATUS = 0x15208,
    L19_ENABLE = 0x15246,
    VREG_READY = 0x80,
    ENABLED = 0x80,
    SUPPLY_US = 2000,
};

/* The PWM at d of PERIOD (the stock driver's M = 1, N and D inverted). */
static bool duty(unsigned d) {
    mmio_write32(GP1_CFG, DUAL_EDGE_XO_BY_4);
    mmio_write32(GP1_M, 1);
    mmio_write32(GP1_N, ~(PERIOD - 1u) & 0xff);
    mmio_write32(GP1_D, ~(d << 1) & 0xff);
    mmio_write32(GP1_CMD, RCG_UPDATE);
    barrier();
    return nexus5_wait_bits(GP1_CMD, RCG_UPDATE, false, WAIT_US);
}

bool nexus5_vibrator_init(void) {
    mmio_write32(nexus5_gpio(ENABLE_PIN) + 4, 0);
    mmio_write32(nexus5_gpio(ENABLE_PIN), PIN_OUTPUT);
    mmio_write32(nexus5_gpio(PWM_PIN), GP1_FUNCTION << 2);
    barrier();
    if (!nexus5_pmic_write(L19_ENABLE, ENABLED)) return false;
    for (uint64_t start = time_us();;) {
        uint8_t status;
        if (nexus5_pmic_read(L19_STATUS, &status) && (status & VREG_READY)) return true;
        if (time_us() - start >= SUPPLY_US) return false;
    }
}

uint32_t nexus5_vibrator_brake(void) { return duty(BRAKE) ? BRAKE_US : 0; }

bool nexus5_vibrator_run(bool on) {
    if (on) {
        if (!duty(FORWARD)) return false;
        mmio_write32(GP1_CBCR, BRANCH_ENABLE);
        barrier();
        if (!nexus5_wait_bits(GP1_CBCR, CLOCK_OFF, false, WAIT_US)) return false;
        mmio_write32(nexus5_gpio(ENABLE_PIN) + 4, PIN_HIGH);
        return true;
    }
    mmio_write32(nexus5_gpio(ENABLE_PIN) + 4, 0);
    mmio_write32(GP1_CBCR, 0);
    barrier();
    return true;
}
