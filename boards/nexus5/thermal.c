#include "nexus5.h"
#include "platform.h"

/* The cores' temperatures from TSENS, as the stock kernel reads them
 * (drivers/thermal/msm8974-tsens.c, calibration fuse_map1). The bootloader
 * leaves all eleven sensors measuring every 62.5 ms; sensors 5 to 8 lie in
 * the four cores (the stock device tree's qcom,cpu-sensors). A sensor's
 * 10-bit code becomes degrees through its calibration in the QFPROM: two
 * points, the codes the part read at 30 and 120 C, or one, with the stock
 * slope of 3.2 codes a degree; parts without any take the stock defaults.
 * (For sensor 8's first point in the backup fuses the stock driver shifts
 * the field out to nothing; it is read here as its mask places it.) */
enum {
    TSENS_CTRL = 0xfc4a8000,
    TSENS_EN = 1u << 0,
    SENSOR_EN_SHIFT = 3,
    TSENS_STATUS = 0xfc4a9030, /* sensor n's at + 4 n */
    TSENS_TRDY = 0xfc4a905c,
    CODE = 0x3ff,
    CALIB = 0xfc4b80d0,  /* six words */
    BACKUP = 0xfc4b8440, /* four words */
    REDUNDANCY = 0xfc4b8444,
    FIRST = 5,
    SENSORS = 4,
    ONE_POINT = 1,
    ONE_POINT_2 = 2,
    TWO_POINT = 3,
    FACTOR = 1000,
    STOCK_SLOPE = 3200, /* codes a degree, times FACTOR */
    POINT1_C = 30,
    POINT2_C = 120,
};

static struct {
    int32_t slope, offset; /* times FACTOR */
} sensor[SENSORS];
static bool calibrated;

bool nexus5_thermal_init(void) {
    if (calibrated) return true;
    uint32_t control = mmio_read32(TSENS_CTRL);
    if (!(control & TSENS_EN) || !nexus5_wait_bits(TSENS_TRDY, 1, true, 1000)) return false;
    for (unsigned i = 0; i < SENSORS; ++i)
        if (!(control & (1u << (SENSOR_EN_SHIFT + FIRST + i)))) return false;
    uint32_t c[6], b[4];
    for (unsigned i = 0; i < 6; ++i) c[i] = mmio_read32(CALIB + 4 * i);
    for (unsigned i = 0; i < 4; ++i) b[i] = mmio_read32(BACKUP + 4 * i);
    unsigned mode, base1, base2, p1[SENSORS], p2[SENSORS];
    if ((mmio_read32(REDUNDANCY) >> 29) == 3) {
        mode = (c[4] >> 30) | ((c[5] >> 28) & 4);
        base1 = b[0] & 0xff;
        p1[0] = (b[1] >> 6) & 0x3f;
        p1[1] = (b[1] >> 12) & 0x3f;
        p1[2] = (b[1] >> 18) & 0x3f;
        p1[3] = b[2] & 0x3f;
        base2 = (b[2] >> 18) & 0x3f;
        p2[0] = (c[4] >> 24) & 0x3f;
        p2[1] = c[5] & 0x3f;
        p2[2] = (c[5] >> 6) & 0x3f;
        p2[3] = (c[5] >> 12) & 0x3f;
    } else {
        mode = (c[1] >> 30) | ((c[3] >> 28) & 4);
        base1 = c[0] & 0xff;
        p1[0] = (c[1] >> 6) & 0x3f;
        p1[1] = (c[1] >> 12) & 0x3f;
        p1[2] = (c[1] >> 18) & 0x3f;
        p1[3] = (c[1] >> 24) & 0x3f;
        base2 = (c[2] >> 12) & 0xff;
        p2[0] = (c[3] >> 18) & 0x3f;
        p2[1] = (c[3] >> 24) & 0x3f;
        p2[2] = c[4] & 0x3f;
        p2[3] = (c[4] >> 6) & 0x3f;
    }
    static const int32_t uncalibrated[SENSORS] = {509, 507, 510, 508};
    for (unsigned i = 0; i < SENSORS; ++i) {
        int32_t point1 = uncalibrated[i], slope = STOCK_SLOPE;
        if (mode == ONE_POINT) point1 = (int32_t)(((base1 << 2) | 3) + p1[i]);
        else if (mode == ONE_POINT_2 || mode == TWO_POINT) point1 = (int32_t)(((base1 + p1[i]) << 2) | 3);
        if (mode == TWO_POINT) {
            int32_t point2 = (int32_t)(((base2 + p2[i]) << 2) | 3);
            slope = (point2 - point1) * FACTOR / (POINT2_C - POINT1_C);
            if (slope <= 0) return false;
        }
        sensor[i].slope = slope;
        sensor[i].offset = point1 * FACTOR - POINT1_C * slope;
    }
    calibrated = true;
    return true;
}

/* The hottest core's temperature in degrees, or false without a sane
 * reading. */
bool nexus5_cpu_temperature(int *degrees) {
    if (!calibrated) return false;
    int hottest = -1000;
    for (unsigned i = 0; i < SENSORS; ++i) {
        int32_t code = (int32_t)(mmio_read32(TSENS_STATUS + 4 * (FIRST + i)) & CODE);
        int32_t value = code * FACTOR - sensor[i].offset, slope = sensor[i].slope;
        int c = value >= 0 ? (value + slope / 2) / slope : -((-value + slope / 2) / slope);
        if (c < -40 || c > 150) return false;
        if (c > hottest) hottest = c;
    }
    *degrees = hottest;
    return true;
}
