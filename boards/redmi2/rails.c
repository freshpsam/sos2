#include "redmi2_rails.h"
#include "redmi2_power_hw.h"
#include "redmi2_display.h"
#include "fs.h"
#include "platform.h"

/* PM8916's regulators, read-only (the RPM owns them), as Linux's
 * qcom_spmi-regulator.c describes them (pm8916_regulators: SID 1, S3 and S4
 * at 0x1a00 and 0x1d00, L1-L18 at 0x4000 + 0x100 n):
 * - TYPE 0x04 is 0x22 for the ULT bucks (SUBTYPE 0x05 ULT_HF_CTL1-4,
 *   0x0d-0x10) and 0x21 for the ULT LDOs (p-type P50-P600 0x08-0x0b and
 *   LV_P50-LV_P450 0x28-0x2d); anything else is not read further;
 * - ENABLE 0x46: 0x80 on; MODE 0x45: 0x80 high power (an LDO's HPM, a
 *   buck's PWM);
 * - VOLTAGE_RANGE 0x40 and VOLTAGE_SET 0x41, range 0 (ult_*_ranges): S3
 *   375 mV + 12.5 mV n, S4 1550 mV + 25 mV n, L4-L18 1750 mV + 12.5 mV n.
 *
 * The bootloader, running on this processor, voted the rails it turned on,
 * so their state is this processor's request, which mainline Linux drops
 * for rails it does not use (msm8916-pm8916.dtsi); nothing lowers the
 * others for sleep. The RPM keeps each processor's active and sleep
 * requests apart (rpm-smd-regulator.c): with no sleep request the active
 * one holds in sleep, and once there is one, it alone does. So each rail
 * standby leaves idle in forced high power gets a sleep request: on, at its
 * voltage restated exactly as read (and within the rail's range in
 * mainline's msm8916-pm8916.dtsi or the WT88047's board file), with no load
 * and no forced mode (the vendor's keys: swen enable, uv microvolts, ma
 * load, lsmd an LDO's mode, 0 by load, ssmd a buck's, 0 automatic), and the
 * RPM picks low power while the chip sleeps. S3, S4 and L5 are held on
 * while awake by Wi-Fi's votes or by the rails they feed; for the others,
 * which may have no request of this processor's but the bootloader's, the
 * awake state is first restated as it is (on, the same voltage, 20 mA:
 * above their high-power threshold, 10 mA or 5 mA, and within the smallest
 * one's 50 mA), so that a sleep request can never be all that holds them.
 * A later change to one of these rails must go to both sets, or whole-chip
 * sleep keeps applying this request. Lowered: S3 and S4 (they feed L1-L3
 * and L4-L7), which the RAM's self-refresh and the idle I/O leave nearly
 * unloaded; L5 (most digital I/O, with the eMMC's and the radio's digital
 * supplies); L6 (the I/O supply of the panel, touch controller and sensors,
 * all asleep or in reset in standby); L11 and L12 (the SD card, unused);
 * L13 (the suspended USB PHY: off, it cost 1 mA more); L16 (the
 * notification LED); and L8 (the eMMC) only in a RAM session, where the boot
 * chain put it to sleep, or an installed phone without its data volume left
 * it idle after reading its system (each of an update's writes ends before
 * the chip sleeps): with persistent storage, an installed phone's data volume
 * included, it stays awake. Off rather than lowered, L6, L8, L11 and L12
 * saved nothing.
 * L17 (the main supply of the panel, touch controller and motion sensor)
 * goes off while the chip sleeps instead, as Linux's drivers turn it off
 * with the screen (the panel's unprepare; edt-ft5x06's power-off suspend,
 * reset first): 8.6 mA lowered, 6.0 mA off. Only if the panel is held in
 * reset when the rails are listed: a panel asleep but out of reset keeps
 * its settings only while powered (this phone's R61308 is held in reset at
 * every standby, display.c). Each wake's active set brings L17 back
 * before anything runs; waking restarts the panel from reset and the touch
 * controller with its reset pulse, and puts the motion sensor, which powers
 * up running, back to sleep (idle_devices.c).
 * The requests go out over Wi-Fi's RPM channel (protected_devices.c), after
 * Wi-Fi's own sleep votes, so they are listed then, from the rails as they
 * are; without Wi-Fi nothing is lowered. */
enum {
    SID = 1,
    TYPE = 0x04,
    SUBTYPE = 0x05,
    RANGE = 0x40,
    SET = 0x41,
    MODE = 0x45,
    ENABLE = 0x46,
    ULT_LDO = 0x21,
    ULT_BUCK = 0x22,
    HIGH = 0x80,
    ON = 0x80,
    SMPA = 0x61706d73,
    LDOA = 0x616f646c,
    SWEN = 0x6e657773,
    UV = 0x7675,
    MA = 0x616d,
    SSMD = 0x646d7373,
    LSMD = 0x646d736c,
    AWAKE_MA = 20,
};

enum kind { LO_SMPS, HO_SMPS, PLDO };

/* held: on while awake through Wi-Fi's votes or the rails it feeds; emmc:
 * only in a RAM session; off: off while the chip sleeps, rather than
 * lowered, with the panel in reset. */
static const struct rail {
    uint8_t peripheral, kind, id;
    bool emmc;
    uint32_t min_uv, max_uv;
    bool held, off;
} rails[] = {
    {0x1a, LO_SMPS, 3, false, 1250000, 1350000, true, false},
    {0x1d, HO_SMPS, 4, false, 1850000, 2150000, true, false},
    {0x44, PLDO, 5, false, 1800000, 1800000, true, false},
    {0x45, PLDO, 6, false, 1800000, 1800000, false, false},
    {0x47, PLDO, 8, true, 2900000, 2900000, false, false},
    {0x4a, PLDO, 11, false, 2950000, 2950000, false, false},
    {0x4b, PLDO, 12, false, 1800000, 2950000, false, false},
    {0x4c, PLDO, 13, false, 3075000, 3075000, false, false},
    {0x4f, PLDO, 16, false, 2800000, 3300000, false, false},
    {0x50, PLDO, 17, false, 2850000, 2850000, false, true},
};

enum { RAILS = sizeof(rails) / sizeof(rails[0]) };
static struct redmi2_rail_request requests[2 * RAILS];
static unsigned request_count;
static bool listed;

static bool known(const struct rail *r, uint8_t type, uint8_t subtype) {
    if (r->kind != PLDO) return type == ULT_BUCK && subtype >= 0x0d && subtype <= 0x10;
    return type == ULT_LDO &&
           ((subtype >= 0x08 && subtype <= 0x0b) || (subtype >= 0x28 && subtype <= 0x2d));
}

/* The rail's voltage in microvolts if it is on in forced high power, else 0. */
static uint32_t high_power_uv(const struct rail *r) {
    static const struct {
        uint32_t min_uv, step_uv, max_set;
    } ranges[] = {[LO_SMPS] = {375000, 12500, 95},
                  [HO_SMPS] = {1550000, 25000, 31},
                  [PLDO] = {1750000, 12500, 127}};

    unsigned ppid = SID << 8 | r->peripheral;
    uint8_t type, subtype, range, set, mode, enable;
    if (!redmi2_power_hw_read(ppid, TYPE, &type) ||
        !redmi2_power_hw_read(ppid, SUBTYPE, &subtype) || !known(r, type, subtype) ||
        !redmi2_power_hw_read(ppid, RANGE, &range) || !redmi2_power_hw_read(ppid, SET, &set) ||
        !redmi2_power_hw_read(ppid, MODE, &mode) || !redmi2_power_hw_read(ppid, ENABLE, &enable) ||
        !(enable & ON) || !(mode & HIGH) || range || set > ranges[r->kind].max_set)
        return 0;
    return ranges[r->kind].min_uv + set * ranges[r->kind].step_uv;
}

unsigned redmi2_rails_requests(const struct redmi2_rail_request **out) {
    if (!listed) {
        listed = true;
        for (unsigned i = 0; i < RAILS; ++i) {
            const struct rail *r = &rails[i];
            if (r->emmc && !fs_session_only()) continue;
            uint32_t uv = high_power_uv(r);
            if (uv < r->min_uv || uv > r->max_uv) continue;
            bool smps = r->kind != PLDO;
            uint32_t type = smps ? SMPA : LDOA;
            if (!r->held)
                requests[request_count++] =
                    (struct redmi2_rail_request){.set = RPM_ACTIVE,
                                                 .type = type,
                                                 .id = r->id,
                                                 .n = 3,
                                                 .kvp = {{SWEN, 1}, {UV, uv}, {MA, AWAKE_MA}}};
            bool off = r->off && redmi2_display_reset_held();
            requests[request_count++] = (struct redmi2_rail_request){
                .set = RPM_SLEEP,
                .type = type,
                .id = r->id,
                .n = 4,
                .kvp = {{SWEN, off ? 0 : 1}, {UV, uv}, {MA, 0}, {smps ? SSMD : LSMD, 0}}};
        }
    }
    *out = requests;
    return request_count;
}
