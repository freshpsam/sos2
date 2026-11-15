#include "qcom_rpm.h"
#include "wire.h"

bool rpm_init(struct rpm_request *r, struct smd_channel *c) {
    if (!r || !c || c->failed) return false;
    *r = (struct rpm_request){.channel = c};
    return true;
}

int rpm_request_kvps(struct rpm_request *r, uint32_t set, uint32_t type, uint32_t id,
                     const uint32_t (*kvp)[2], unsigned n, uint64_t now) {
    if (!r || !kvp || !n || n > RPM_KVPS || r->failed || r->pending || r->sequence == UINT32_MAX ||
        set > RPM_SLEEP || now > UINT64_MAX - 5000000)
        return -1;
    unsigned char packet[28 + 12 * RPM_KVPS] = {0};
    unsigned size = 28 + 12 * n;
    put32(packet, 0x00716572);
    put32(packet + 4, size - 8);
    put32(packet + 8, r->sequence + 1);
    put32(packet + 12, set);
    put32(packet + 16, type);
    put32(packet + 20, id);
    put32(packet + 24, 12 * n);
    for (unsigned i = 0; i < n; ++i) {
        unsigned char *p = packet + 28 + 12 * i;
        put32(p, kvp[i][0]);
        put32(p + 4, 4);
        put32(p + 8, kvp[i][1]);
    }
    int result = smd_send(r->channel, packet, size, now);
    if (result == 1) {
        ++r->sequence;
        r->pending = true;
        r->deadline = now + 5000000;
    }
    if (result < 0) r->failed = true;
    return result;
}

int rpm_request(struct rpm_request *r, uint32_t set, uint32_t type, uint32_t id, uint32_t key,
                uint32_t value, uint64_t now) {
    const uint32_t kvp[1][2] = {{key, value}};
    return rpm_request_kvps(r, set, type, id, kvp, 1, now);
}

int rpm_vote(struct rpm_request *r, uint32_t type, uint32_t id, uint32_t key, uint32_t value,
             uint64_t now) {
    return rpm_request(r, RPM_ACTIVE, type, id, key, value, now);
}

int rpm_poll(struct rpm_request *r, uint64_t now) {
    if (!r || r->failed) return -1;
    if (!r->pending) return 1;
    unsigned char packet[256];
    unsigned size = 0;
    int result = smd_receive(r->channel, packet, sizeof(packet), &size, now);
    if (!result) {
        if (now >= r->deadline) goto failed;
        return 0;
    }
    if (result < 0 || size < 20 || le32(packet) != 0x00716572 || le32(packet + 4) != size - 8)
        goto failed;
    bool have_id = false, rejected = false;
    uint32_t message_id = 0;
    for (unsigned offset = 8; offset < size;) {
        if (size - offset < 8) goto failed;
        uint32_t key = le32(packet + offset), n = le32(packet + offset + 4);
        offset += 8;
        if (n > size - offset || n > UINT32_MAX - 3) goto failed;
        if (key == 0x2367736d) {
            if (n != 4 || have_id) goto failed;
            have_id = true;
            message_id = le32(packet + offset);
        } else if (key == 0x727265) rejected = true;
        else goto failed; /* Unknown KVPs cannot establish success. */
        unsigned padded = (n + 3) & ~3u;
        if (padded > size - offset) goto failed;
        offset += padded;
    }
    if (!have_id) goto failed;
    /* The vendor dispatches by message ID and ignores replies without a
     * waiting request (including old/sleep-set ACKs). Keep a fixed deadline
     * and consume just one packet per poll so noise cannot stall the desktop. */
    if (message_id != r->sequence) {
        if (now >= r->deadline) goto failed;
        return 0;
    }
    r->pending = false; /* Answered. */
    if (rejected) r->failed = true;
    return rejected ? -1 : 1;
failed:
    r->failed = true;
    return -1; /* Unanswered: the request may still be in flight. */
}
