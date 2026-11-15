#include "redmi2_power_hw.h"
#include <stdint.h>

/* Original implementation from primary hardware descriptions:
 * WT88047 wiring, pinned vendor kernel (MPP4/DTEST1 and PWM panel variants):
 * https://github.com/LineageOS/android_kernel_wingtech_msm8916/blob/beb5692ed25808aebce7b57e318cf8bd33e645d8/arch/arm/boot/dts/qcom/wt88047/msm8916-wt88047.dtsi
 * lk2nd 23.1 target/msm8916/target_display.c, dev/pmic/pm8x41/pm_pwm.c,
 * dev/pmic/pm8x41/include/pm_pwm.h, dev/pmic/pm8x41/pm8x41.c:
 * https://github.com/msm8916-mainline/lk2nd/tree/23.1
 * Linux drivers/{spmi/spmi-pmic-arb.c,input/misc/pm8941-pwrkey.c,
 * pinctrl/qcom/pinctrl-spmi-mpp.c,leds/rgb/leds-qcom-lpg.c}:
 * https://github.com/torvalds/linux/tree/master/drivers
 *
 * This is PWM through PM8916 MPP4, NOT WLED. Continuous-splash firmware owns
 * panel initialization. Keep its duty, frequency, DTEST routing and supply
 * configuration; gate two enable bits and re-latch the saved PWM duty on wake.
 * No panel clock or shared rail is disabled. All SPMI accesses are polled v2,
 * execution environment zero.
 *
 * PWM re-enable requires rewriting duty AFTER enable, then SYNC, even when
 * the duty register values have not changed (documented LPG hardware bug):
 * https://github.com/LineageOS/android_kernel_wingtech_msm8916/blob/beb5692ed25808aebce7b57e318cf8bd33e645d8/drivers/pwm/pwm-qpnp.c#L1112-L1157
 * The same file's qpnp_lpg_save_pwm_value() applies PWM-only subtype SYNC.
 */
#include "platform.h"

static uint32_t read32(uintptr_t address) {
    uint32_t value = mmio_read32(address);
    barrier();
    return value;
}

static void write32(uintptr_t address, uint32_t value) {
    barrier();
    mmio_write32(address, value);
    barrier();
}

#define now_us time_us

enum {
    ARB_CORE = 0x0200f000,
    ARB_WRITE = 0x02400000,
    ARB_READ = 0x02c00000,
    APIDS = 128,
    CHANNEL_STRIDE = 0x8000,
    TIMEOUT_US = 1000,
    POLL_LIMIT = 100000,
    PON_PPID = 0x008,
    MPP4_PPID = 0x0a3,
    PWM_PPID = 0x1bc,
    TYPE = 4,
    SUBTYPE = 5,
    RT_STATUS = 0x10,
    MODE = 0x40,
    CLOCK_VIN = 0x41,
    VALUE_LOW = 0x44,
    VALUE_HIGH = 0x45,
    ENABLE = 0x46,
    SYNC = 0x47,
    ENABLE_BIT = 0x80,
    /* Interrupts, v2 arbiter (Linux spmi-pmic-arb.c): the ownership table in
     * its configuration block; per APID, the accumulator enable, status and
     * clear; this processor's (owner 0) accumulated status. */
    ARB_CNFG = 0x0200a000,
    OWNER_TABLE = 0x700,
    ARB_INTR = 0x03800000,
    APID_STRIDE = 0x1000,
    ACC_ENABLE = 0,
    IRQ_STATUS = 4,
    IRQ_CLEAR = 8,
    OWNER_ACC = 0x100000,
    /* PMIC peripheral interrupt registers; the power key is PON interrupt 0. */
    INT_SET_TYPE = 0x11,
    INT_POLARITY_HIGH = 0x12,
    INT_POLARITY_LOW = 0x13,
    INT_LATCHED_CLR = 0x14,
    INT_EN_SET = 0x15,
    INT_EN_CLR = 0x16,
    INT_LATCHED_STS = 0x18,
    KEY_IRQ = 1,
};

static unsigned pon_apid, mpp_apid, pwm_apid;
static bool key_armed;
static uint8_t saved_mpp_enable, saved_pwm_enable, saved_mpp_mode;
static uint8_t saved_duty_low, saved_duty_high, duty_low_mask;
static bool ready, restore_pending;

static bool lookup_apid(unsigned ppid, unsigned *apid) {
    for (unsigned i = 0; i < APIDS; ++i) {
        uint32_t entry = read32(ARB_CORE + 0x800 + 4 * i);
        if (entry && ((entry >> 8) & 0xfff) == ppid) {
            *apid = i;
            return true;
        }
    }
    return false;
}

static bool find_apid(unsigned ppid, unsigned *apid) { return lookup_apid(ppid, apid); }

static bool access(unsigned apid, uint8_t reg, uint8_t *value, bool write) {
    uintptr_t base = (write ? ARB_WRITE : ARB_READ) + CHANNEL_STRIDE * apid;
    write32(base + 4, 0);
    if (write) write32(base + 0x10, *value);
    /* Extended-long byte transfer: byte-count field is length minus one. */
    write32(base, (write ? 0u : 1u << 27) | ((uint32_t)reg << 4));
    uint64_t start = now_us();
    for (unsigned i = 0; i < POLL_LIMIT; ++i) {
        uint32_t status = read32(base + 8);
        if (status & 1) {
            if (status & 4) return false;   /* SPMI access denied */
            if (status & 0xa) return false; /* SPMI transfer failed */
            if (!write) *value = (uint8_t)read32(base + 0x18);
            return true;
        }
        if (now_us() - start >= TIMEOUT_US) break;
    }
    return false; /* SPMI timed out */
}

static bool transfer(unsigned apid, uint8_t reg, uint8_t *value, bool write) {
    return access(apid, reg, value, write);
}

static bool get(unsigned apid, uint8_t reg, uint8_t *value) {
    return transfer(apid, reg, value, false);
}

static bool set_enable(unsigned apid, uint8_t saved) {
    uint8_t before, after;
    if (!get(apid, ENABLE, &before)) return false;
    uint8_t desired = (before & ~ENABLE_BIT) | (saved & ENABLE_BIT);
    if (desired != before && !transfer(apid, ENABLE, &desired, true)) return false;
    if (!get(apid, ENABLE, &after)) return false;
    return after == desired;
}

static bool rewrite_duty(uint8_t reg, uint8_t mask, uint8_t saved) {
    uint8_t before, after;
    if (!get(pwm_apid, reg, &before)) return false;
    uint8_t desired = (before & ~mask) | (saved & mask);
    /* The write itself is required by the erratum; equality is not a reason
     * to skip it. Preserve reserved bits, including the unused 6-bit PWM bits. */
    if (!transfer(pwm_apid, reg, &desired, true) || !get(pwm_apid, reg, &after)) return false;
    return after == desired;
}

static bool latch_pwm(void) {
    if (!rewrite_duty(VALUE_LOW, duty_low_mask, saved_duty_low) ||
        !rewrite_duty(VALUE_HIGH, 1, saved_duty_high))
        return false;
    uint8_t trigger = 1;
    /* SYNC is a command strobe and may self-clear. Check the SPMI transaction,
     * not persistent register equality; do not read/modify/write this trigger. */
    return transfer(pwm_apid, SYNC, &trigger, true);
}

static bool restore(void) {
    restore_pending = true;
    /* Enable -> forced duty rewrite -> SYNC -> MPP4. Register enable readback
     * alone cannot prove that the PWM output resumed. Still attempt MPP4 after
     * a PWM failure, and retain recovery state for the caller's bounded retry.
     * MPP4's route first, in case standby drove it low. */
    bool pwm_ok = set_enable(pwm_apid, saved_pwm_enable);
    if (pwm_ok) pwm_ok = latch_pwm();
    uint8_t mode;
    bool mpp_ok = get(mpp_apid, MODE, &mode);
    if (mpp_ok && mode != saved_mpp_mode) {
        uint8_t route = saved_mpp_mode;
        mpp_ok = transfer(mpp_apid, MODE, &route, true);
    }
    mpp_ok = set_enable(mpp_apid, saved_mpp_enable) && mpp_ok;
    if (pwm_ok && mpp_ok) restore_pending = false;
    return pwm_ok && mpp_ok;
}

bool redmi2_power_hw_init(void) {
    if (ready && restore_pending)
        return false; /* restore backlight before power reinitialization */
    ready = false;
    uint32_t version = read32(ARB_CORE);
    if (version < 0x20010000u || version >= 0x30000000u)
        return false; /* power requires MSM8916 SPMI v2 */
    if (!find_apid(PON_PPID, &pon_apid) || !find_apid(MPP4_PPID, &mpp_apid) ||
        !find_apid(PWM_PPID, &pwm_apid))
        return false;
    uint8_t type, subtype, mode, vin, pwm_subtype, clock, duty_low, duty_high, key;
    if (!get(pon_apid, RT_STATUS, &key) || !get(mpp_apid, TYPE, &type) ||
        !get(mpp_apid, SUBTYPE, &subtype) || !get(mpp_apid, MODE, &mode) ||
        !get(mpp_apid, CLOCK_VIN, &vin) || !get(mpp_apid, ENABLE, &saved_mpp_enable) ||
        !get(pwm_apid, SUBTYPE, &pwm_subtype) || !get(pwm_apid, CLOCK_VIN, &clock) ||
        !get(pwm_apid, VALUE_LOW, &duty_low) || !get(pwm_apid, VALUE_HIGH, &duty_high) ||
        !get(pwm_apid, ENABLE, &saved_pwm_enable))
        return false;
    if (type != 0x11 || !((subtype >= 3 && subtype <= 7) || subtype == 0x0f))
        return false; /* unsupported PM8916 MPP4 type/subtype */
    /* Digital output (1), DTEST1 (4<<1), non-inverted, VIN0. */
    if ((mode & 0x7f) != 0x18 || (vin & 7) != 0)
        return false; /* inherited MPP4 backlight route differs */
    saved_mpp_mode = mode;
    if (pwm_subtype != 0x0b) return false; /* unsupported PM8916 PWM subtype */
    unsigned duty = clock & 4 ? duty_low | ((duty_high & 1u) << 8) : duty_low & 0x3f;
    if (!(clock & 3) || !duty || !(saved_mpp_enable & ENABLE_BIT) ||
        !(saved_pwm_enable & ENABLE_BIT))
        return false; /* inherited PWM backlight is not live */
    saved_duty_low = duty_low;
    saved_duty_high = duty_high;
    duty_low_mask = clock & 4 ? 0xff : 0x3f;
    restore_pending = false;
    ready = true;
    return true;
}

bool redmi2_power_hw_read(unsigned ppid, uint8_t reg, uint8_t *value) {
    unsigned apid;
    return ready && value && lookup_apid(ppid, &apid) && access(apid, reg, value, false);
}

bool redmi2_power_hw_write(unsigned ppid, uint8_t reg, uint8_t value) {
    unsigned apid;
    return ready && lookup_apid(ppid, &apid) && access(apid, reg, &value, true);
}

bool redmi2_power_hw_read_key(bool *pressed) {
    if (!ready) return false;   /* power hardware not initialized */
    if (!pressed) return false; /* power key output is null */
    uint8_t status;
    if (!get(pon_apid, RT_STATUS, &status)) return false;
    *pressed = !!(status & 1); /* PON_KPDPWR_N_SET means pressed, not released. */
    return true;
}

/* As Linux sets the power key's interrupt up (spmi-pmic-arb.c qpnpint_*,
 * pm8941-pwrkey.c: both edges): only if this processor owns it; disabled and
 * cleared, edge-triggered on both polarities, the arbiter's accumulator on,
 * then cleared again and enabled in the PMIC. */
bool redmi2_power_hw_key_irq(void) {
    key_armed = false;
    if (!ready) return false;
    if (read32(ARB_CNFG + OWNER_TABLE + 4 * pon_apid) & 7)
        return false; /* the power key's interrupt belongs to another processor */
    uint8_t bit = KEY_IRQ, type, high, low;
    if (!transfer(pon_apid, INT_EN_CLR, &bit, true) ||
        !transfer(pon_apid, INT_LATCHED_CLR, &bit, true) || !get(pon_apid, INT_SET_TYPE, &type) ||
        !get(pon_apid, INT_POLARITY_HIGH, &high) || !get(pon_apid, INT_POLARITY_LOW, &low))
        return false;
    type |= KEY_IRQ;
    high |= KEY_IRQ;
    low |= KEY_IRQ;
    if (!transfer(pon_apid, INT_SET_TYPE, &type, true) ||
        !transfer(pon_apid, INT_POLARITY_HIGH, &high, true) ||
        !transfer(pon_apid, INT_POLARITY_LOW, &low, true))
        return false;
    write32(ARB_INTR + APID_STRIDE * pon_apid + ACC_ENABLE, 1);
    if (!transfer(pon_apid, INT_LATCHED_CLR, &bit, true) ||
        !transfer(pon_apid, INT_EN_SET, &bit, true))
        return false;
    key_armed = true;
    return true;
}

bool redmi2_power_hw_key_rearm(void) {
    if (!key_armed) return false;
    /* Another peripheral interrupt of this processor's would hold the shared
     * line up: nothing here handles one, so its accumulator goes off. */
    for (unsigned n = 0; n < APIDS / 32; ++n) {
        uint32_t pending = read32(ARB_INTR + OWNER_ACC + 4 * n);
        for (unsigned bit = 0; bit < 32; ++bit) {
            unsigned apid = 32 * n + bit;
            if (!(pending & (1u << bit)) || apid == pon_apid) continue;
            write32(ARB_INTR + APID_STRIDE * apid + ACC_ENABLE, 0);
        }
    }
    /* Linux's qpnpint_irq_ack: the arbiter's copy, then the PMIC's latch
     * (the key's always, in case the arbiter missed it). */
    uintptr_t pon = ARB_INTR + APID_STRIDE * pon_apid;
    uint32_t latched = read32(pon + IRQ_STATUS) & 0xff;
    if (latched) write32(pon + IRQ_CLEAR, latched);
    uint8_t clear = (uint8_t)(latched | KEY_IRQ);
    return transfer(pon_apid, INT_LATCHED_CLR, &clear, true);
}

bool redmi2_power_hw_key_latched(void) {
    if (!key_armed) return false;
    if (read32(ARB_INTR + APID_STRIDE * pon_apid + IRQ_STATUS) & KEY_IRQ) return true;
    uint8_t latched;
    return get(pon_apid, INT_LATCHED_STS, &latched) && (latched & KEY_IRQ);
}

bool redmi2_power_hw_set_backlight(bool enabled) {
    if (!ready) return false; /* power hardware not initialized */
    if (enabled) return restore();
    /* Match lk2nd's off order, preserving all other bits. Mark restore pending
     * before the first write, since even an errored transfer may have applied. */
    restore_pending = true;
    if (set_enable(pwm_apid, 0) && set_enable(mpp_apid, 0)) {
        /* Then MPP4, the backlight driver's enable, driven low instead of
         * left undriven, as the vendor kernel leaves it (the LPG off, the
         * MPP still enabled): a digital output (1) of a constant (0), not
         * inverted. The backlight is off either way; restore puts the PWM
         * route back first. */
        uint8_t low = 0x10, on = ENABLE_BIT;
        (void)(transfer(mpp_apid, MODE, &low, true) && transfer(mpp_apid, ENABLE, &on, true));
        return true;
    }
    (void)restore(); /* Keep the original failure detail through cleanup. */
    return false;
}
