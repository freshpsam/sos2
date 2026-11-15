#ifndef SOS2_ARM_MACHINE_H
#define SOS2_ARM_MACHINE_H
#include "fdt.h"

struct arm_timer_route {
    uintptr_t distributor, cpu;
    unsigned interrupt;
    bool qcom;
};

struct arm_mapping {
    uintptr_t start;
    uint32_t size;
};

struct arm_machine {
    struct arm_timer_route timer;
    uintptr_t rtc;
    struct arm_mapping devices[MAX_VIRTIO + 32];
    unsigned device_count;
};

/* Board description only: no MMIO reads or writes. */
bool arm_machine_init(const struct platform_devices *devices);
const struct arm_machine *arm_machine_get(void);

/* Stable identifiers used in desktop calibration records. PMIC seconds have
 * no assumed epoch. Reads never write the hardware counter. */
enum arm_rtc_kind { ARM_RTC_UNIX = 1, ARM_RTC_PMIC = 2 };
enum arm_rtc_kind arm_rtc_kind(void);
bool arm_rtc_read(uint32_t *seconds);

bool arm_timer_init(const struct arm_timer_route *route);
bool arm_timer_secondary_init(void);
const char *arm_timer_error(void);
void arm_timer_arm_us(uint32_t microseconds);
void arm_timer_stop(bool acknowledge);
/* Standby only: CPU 0 sleeps for up to `microseconds` (1 s at most). */
void arm_cpu_sleep_us(uint32_t microseconds);
#endif
