#ifndef SOS2_SYSTEM_CLOCK_H
#define SOS2_SYSTEM_CLOCK_H
#include "arm_machine.h"
/* Wall time may be corrected; monotonic time, sleeps and CPU accounting never
 * use this service. No user syscall can set time or access its saved record. */
#define SYSTEM_CLOCK_MIN_SECONDS 946684800ull
#define SYSTEM_CLOCK_END_SECONDS 4102444800ull

struct system_clock_ops {
    bool (*read)(uint32_t *);
    enum arm_rtc_kind kind;
};

void system_clock_init(const struct system_clock_ops *);
/* Whether the wall time is known. */
bool system_clock_ready(void);
bool system_clock_read(uint64_t *microseconds);
/* UTC at receipt, in microseconds; saving RTC calibration is best effort. */
bool system_clock_synchronize(uint64_t microseconds);
bool system_clock_pending(void);
void system_clock_service(void *);
#endif
