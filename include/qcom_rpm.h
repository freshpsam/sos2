#ifndef SOS2_QCOM_RPM_H
#define SOS2_QCOM_RPM_H
#include "qcom_smd.h"

struct rpm_request {
    struct smd_channel *channel;
    uint32_t sequence;
    uint64_t deadline;
    bool pending, failed;
};

bool rpm_init(struct rpm_request *, struct smd_channel *);

/* Single key/value vote for the active set (RPM_ACTIVE) or for the set the
 * RPM applies while the application processor sleeps with the RPM notified
 * (RPM_SLEEP; without one, the active vote holds then too). Poll returns 1
 * only for matching success; a failure leaves the request pending unless the
 * RPM answered (rejected it), since one never answered may still be in
 * flight. */
enum { RPM_ACTIVE = 0, RPM_SLEEP = 1, RPM_KVPS = 4 };

int rpm_request(struct rpm_request *, uint32_t set, uint32_t type, uint32_t id, uint32_t key,
                uint32_t value, uint64_t now);
/* Up to RPM_KVPS key/value pairs in one request, which the RPM applies
 * together: a resource's state can change several keys at once. */
int rpm_request_kvps(struct rpm_request *, uint32_t set, uint32_t type, uint32_t id,
                     const uint32_t (*kvp)[2], unsigned n, uint64_t now);
int rpm_vote(struct rpm_request *, uint32_t type, uint32_t id, uint32_t key, uint32_t value,
             uint64_t now);
int rpm_poll(struct rpm_request *, uint64_t now);
#endif
