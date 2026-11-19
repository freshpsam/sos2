#ifndef SOS2_WCNSS_POWER_H
#define SOS2_WCNSS_POWER_H
#include "qcom_rpm.h"

/* Withdraws the power requests wcnss_boot.c makes to start the WCNSS, once its
 * firmware runs and requests what it needs itself; Linux's qcom_wcnss.c drops
 * them after the ready signal. Start zeroed and call until it returns 1
 * (done) or -1 (the power controller refused a request or had no room for
 * 5 s: the remaining requests stay). 0 means a request is still pending. */
struct wcnss_power_release {
    unsigned step;
    uint64_t deadline;
};

int wcnss_power_release(struct wcnss_power_release *, struct rpm_request *, uint64_t now);

#endif
