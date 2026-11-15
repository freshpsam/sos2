#include "wcnss_power.h"

/* RPM resources and keys as wcnss_boot.c's start-up votes use them: LDO
 * (ldoa), SMPS (smpa) and clock buffer (clka) resources; load in mA (ma),
 * software enable (swen) and voltage corner (corn).
 * - The PA supply (L9) and RF_CLK2 serve only the radio: they turn off.
 * - S3 (which feeds L2, the RAM's supply) and L7 (the CPU PLL) are shared,
 *   and mainline Linux keeps them always on (msm8916-pm8916.dtsi): they
 *   stay enabled and lose only their load, so the power controller may pick
 *   a low-power mode;
 * - L5 (most digital I/O, with the eMMC's I/O and the radio's digital
 *   supply) keeps its load: low power could not carry the eMMC's bursts.
 *   Whole-chip sleep lowers it (rails.c).
 * - The CX and MX core rails lose their corner and enable, as Linux's rpmpd
 *   releases them; lk2nd votes neither, and SOS2 runs without them when it
 *   starts no Wi-Fi.
 * The XO source vote stays for the active set; the sleep set drops it
 * (protected_devices.c). */
enum {
    LDOA = 0x616f646c,
    SMPA = 0x61706d73,
    CLKA = 0x616b6c63,
    LOAD = 0x616d,
    ENABLE = 0x6e657773,
    CORNER = 0x6e726f63,
    STEPS = 9,
    TIMEOUT_US = 5000000
};

static const uint32_t steps[STEPS][4] = {
    {LDOA, 9, LOAD, 0},   {LDOA, 9, ENABLE, 0}, /* PA */
    {CLKA, 5, ENABLE, 0},                       /* RF_CLK2 */
    {SMPA, 3, LOAD, 0},   {LDOA, 7, LOAD, 0},   /* RFA, PX/XO: on */
    {SMPA, 1, CORNER, 0}, {SMPA, 1, ENABLE, 0}, /* CX */
    {LDOA, 3, CORNER, 0}, {LDOA, 3, ENABLE, 0}, /* MX */
};

int wcnss_power_release(struct wcnss_power_release *r, struct rpm_request *rpm, uint64_t now) {
    if (r->step == STEPS) return 1;
    if (!rpm || rpm->failed) return -1;
    if (rpm->pending) {
        int result = rpm_poll(rpm, now);
        if (result <= 0) return result;
        r->deadline = 0;
        return ++r->step == STEPS;
    }
    if (!r->deadline) r->deadline = now + TIMEOUT_US;
    else if (now >= r->deadline) return -1; /* Never room to send. */
    const uint32_t *s = steps[r->step];
    return rpm_vote(rpm, s[0], s[1], s[2], s[3], now) < 0 ? -1 : 0;
}
