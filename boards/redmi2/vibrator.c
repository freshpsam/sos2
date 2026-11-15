#include "redmi2_vibrator.h"
#include "redmi2_power_hw.h"
#include <stdint.h>

/* The PM8916's vibrator driver, which powers the handset's coin motor: SID 1
 * at 0xc000, as the WT88047's vendor kernel drives it (qpnp-vibrator.c in
 * manual mode, the default; msm8916-wt88047.dts sets 3000 mV for all its
 * uses), Linux's
 * pm8xxx-vibrator.c (pm8916_regs) and lk2nd's pm_vib.h (0x1c041, 0x1c046):
 * - VTG_CTL 0x41, bits 4:0: the drive voltage in 100 mV steps, 1.2 to 3.1 V;
 * - EN_CTL 0x46: bit 7 runs the motor. Bit 4 makes the drive active-low and
 *   bits 2:0 hand it to a DTEST line that a PWM drives; the handset's board
 *   file sets neither. The other bits of both registers are kept, as the
 *   vendor driver keeps them.
 * lk2nd (PON_VIB_SUPPORT) buzzes 250 ms at 2.2 V as it starts and waits for
 * that to end before it boots a kernel, so the motor is normally found off.
 * https://github.com/LineageOS/android_kernel_wingtech_msm8916/blob/beb5692ed25808aebce7b57e318cf8bd33e645d8/drivers/platform/msm/qpnp-vibrator.c
 * https://github.com/LineageOS/android_kernel_wingtech_msm8916/blob/beb5692ed25808aebce7b57e318cf8bd33e645d8/arch/arm/boot/dts/qcom/msm8916-wt88047.dts
 * https://github.com/torvalds/linux/blob/master/drivers/input/misc/pm8xxx-vibrator.c
 * https://github.com/msm8916-mainline/lk2nd/blob/3e86db90765468c55e1be507cb897efd20876b71/dev/pmic/pm8x41/include/pm_vib.h
 */
enum {
    PPID = 0x1c0,
    VTG_CTL = 0x41,
    EN_CTL = 0x46,
    LEVEL_MASK = 0x1f,
    RUN = 0x80,
    PWM_DRIVE = 0x17,
};

static uint8_t stopped; /* EN_CTL with the motor off */

static bool set(uint8_t reg, uint8_t value) {
    uint8_t actual;
    return redmi2_power_hw_write(PPID, reg, value) && redmi2_power_hw_read(PPID, reg, &actual) &&
           actual == value;
}

bool redmi2_vibrator_init(unsigned millivolts) {
    if (millivolts < REDMI2_VIBRATOR_MIN_MV || millivolts > REDMI2_VIBRATOR_MAX_MV ||
        millivolts % 100)
        return false;
    uint8_t enable, voltage, level = (uint8_t)(millivolts / 100);
    if (!redmi2_power_hw_read(PPID, EN_CTL, &enable)) return false;
    stopped = (uint8_t)(enable & ~RUN);
    if ((enable & RUN) && !set(EN_CTL, stopped)) return false;
    if (stopped & PWM_DRIVE) return false; /* not manual mode */
    if (!redmi2_power_hw_read(PPID, VTG_CTL, &voltage) ||
        !set(VTG_CTL, (uint8_t)((voltage & ~LEVEL_MASK) | level)))
        return false;
    return true;
}

/* Only a stop needs proof: a start that did not take is merely silent. */
bool redmi2_vibrator_run(bool on) {
    if (on) return redmi2_power_hw_write(PPID, EN_CTL, (uint8_t)(stopped | RUN));
    return set(EN_CTL, stopped);
}
