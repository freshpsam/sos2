#ifndef SOS2_REDMI2_RAILS_H
#define SOS2_REDMI2_RAILS_H
#include <stdbool.h>
#include "qcom_rpm.h"

/* The PMIC rails standby leaves idle in forced high power (PM8916 S3, S4,
 * L5, L6, L8, L11-L13, L16, L17), read through the SPMI arbiter; nothing is
 * written. Each gets RPM requests for the sleep set: on at the same
 * voltage, no load, no forced mode, so the RPM can lower it while the chip
 * sleeps; L17 instead goes off then, if the panel is held in reset
 * (redmi2_display_reset_held) when they are listed. Awake they stay as they
 * are (restated as they are for the active set first, unless Wi-Fi's votes
 * or the rails they feed hold them). They go out over Wi-Fi's RPM channel
 * after Wi-Fi's own sleep votes (protected_devices.c):
 * redmi2_rails_requests reads the rails and lists them then, once a boot. */
struct redmi2_rail_request {
    uint32_t set, type, id;
    unsigned n;
    uint32_t kvp[RPM_KVPS][2];
};

unsigned redmi2_rails_requests(const struct redmi2_rail_request **requests);

#endif
