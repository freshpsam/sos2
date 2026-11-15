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

enum clock_network_state {
    CLOCK_NETWORK_OFFLINE,
    CLOCK_NETWORK_SYNCING,
    CLOCK_NETWORK_RETRY,
    CLOCK_NETWORK_SYNCED,
    CLOCK_NETWORK_REFUSED
};

struct system_clock_view {
    bool ready, busy, storage, rtc_available, saved;
    enum clock_network_state network;
    const char *message;
};

void system_clock_init(const struct system_clock_ops *);
const struct system_clock_view *system_clock_view(void);
bool system_clock_read(uint64_t *microseconds);
/* UTC at receipt, in microseconds; saving RTC calibration is best effort. */
bool system_clock_synchronize(uint64_t microseconds);
void system_clock_network_status(enum clock_network_state);
bool system_clock_pending(void);
void system_clock_service(void *);
#endif
