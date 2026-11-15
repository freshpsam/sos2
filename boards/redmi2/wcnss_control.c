#include "wcnss_control.h"
#include "platform.h"

enum {
    VERSION_REQUEST = 0x01000000,
    VERSION_RESPONSE = 0x01000001,
    NV_REQUEST = 0x01000002,
    NV_RESPONSE = 0x01000003,
    BOOT_COMPLETE = 0x0100000c,
    FRAGMENT = 3072,
    NV_LIMIT = 1024 * 1024
};

static uint32_t word(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put(unsigned char *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (8 * i));
}

static enum wcnss_state fail(struct wcnss_control *c, const char *why) {
    c->error = why;
    c->state = WCNSS_FAILED;
    smd_close(c->channel);
    return c->state;
}

static enum wcnss_state timed_out(struct wcnss_control *c) {
    const char *why = c->state == WCNSS_BOOT_WAIT ? "WCNSS calibration timed out"
                      : c->state == WCNSS_VERSION_SEND || c->state == WCNSS_VERSION_WAIT
                          ? "WCNSS version query timed out"
                          : "WCNSS board-data upload timed out";
    return fail(c, why);
}

bool wcnss_control_start(struct wcnss_control *c, struct smd_channel *channel, const void *nv,
                         unsigned size, uint64_t now) {
    if (!c) return false;
    *c = (struct wcnss_control){.state = WCNSS_FAILED, .error = "WCNSS board data invalid"};
    /* The control channel carries a 9-byte NV response, so its FIFO must allow
     * byte packets. NV is opaque board data; never manufacture generic defaults. */
    if (!channel || channel->failed || channel->words || channel->size < 4096 || !nv || size < 5 ||
        size > NV_LIMIT || now > UINT64_MAX - 30000000)
        return false;
    /* The stock Redmi 2 driver omits the file's validity bitmap from both
     * WCNSS_CTRL and WLAN_CTRL uploads. Keep the packaged file unchanged. */
    c->channel = channel;
    c->nv = (const unsigned char *)nv + 4;
    c->size = size - 4;
    c->state = WCNSS_VERSION_SEND;
    c->error = 0;
    return true;
}

enum wcnss_state wcnss_control_poll(struct wcnss_control *c, uint64_t now) {
    if (!c || c->state == WCNSS_FAILED) return WCNSS_FAILED;
    if (now > UINT64_MAX - 30000000) return fail(c, "WCNSS control clock overflow");
    int result = smd_poll(c->channel, now);
    if (result < 0) return fail(c, "WCNSS control link lost");
    if (result == SMD_AGAIN) return c->state; /* SMD owns its opening deadline. */
    if (!c->deadline) c->deadline = now + 10000000;
    /* Cold-boot completion is unsolicited: it may precede the NV response,
     * arrive between fragments, or follow an acknowledgement that needed no wait. */
    bool notification = false;
    /* Look past queued indications for the reply before checking its deadline.
     * The fixed receive budget also bounds an unsolicited-notification flood. */
    for (unsigned budget = 0; budget < 8; ++budget) {
        unsigned size = 0;
        result = smd_receive(c->channel, c->packet, sizeof(c->packet), &size, now);
        if (result < 0) return fail(c, "WCNSS control receive failed");
        if (result == SMD_AGAIN) break;
        if (size < 8 || word(c->packet + 4) != size)
            return fail(c, "malformed WCNSS control reply");
        uint32_t type = word(c->packet);
        if (type == BOOT_COMPLETE && size == 8) {
            c->cold_boot_done = true;
            if (c->state == WCNSS_BOOT_WAIT) {
                c->state = WCNSS_READY;
                return c->state;
            }
            notification = true;
            continue;
        } else if (c->state == WCNSS_VERSION_WAIT && type == VERSION_RESPONSE && size == 12) {
            memcpy(c->version, c->packet + 8, 4);
            c->state = WCNSS_NV_SEND;
            c->deadline = now + 30000000;
        } else if (c->state == WCNSS_NV_WAIT && type == NV_RESPONSE && size == 9) {
            unsigned status = c->packet[8];
            if (status != 1 && status != 2) return fail(c, "WCNSS board data rejected");
            c->state = status == 1 || c->cold_boot_done ? WCNSS_READY : WCNSS_BOOT_WAIT;
            c->deadline = now + 10000000;
        } else return fail(c, "unexpected WCNSS control reply");
        return c->state;
    }
    if (c->state != WCNSS_READY && now >= c->deadline) return timed_out(c);
    if (notification) return c->state;
    if (c->state == WCNSS_VERSION_SEND) {
        put(c->packet, VERSION_REQUEST);
        put(c->packet + 4, 8);
        result = smd_send(c->channel, c->packet, 8, now);
        if (result < 0) return fail(c, "WCNSS version query send failed");
        if (result == SMD_OK) {
            c->state = WCNSS_VERSION_WAIT;
            c->deadline = now + 10000000;
        }
        return c->state;
    }
    if (c->state == WCNSS_NV_SEND) {
        unsigned bytes = c->size - c->sent;
        if (bytes > FRAGMENT) bytes = FRAGMENT;
        put(c->packet, NV_REQUEST);
        put(c->packet + 4, bytes + 16);
        c->packet[8] = (unsigned char)c->sequence;
        c->packet[9] = (unsigned char)(c->sequence >> 8);
        c->packet[10] = c->sent + bytes == c->size;
        c->packet[11] = 0;
        put(c->packet + 12, bytes);
        memcpy(c->packet + 16, c->nv + c->sent, bytes);
        result = smd_send(c->channel, c->packet, bytes + 16, now);
        if (result < 0) return fail(c, "WCNSS board-data send failed");
        if (result == SMD_OK) {
            c->sent += bytes;
            ++c->sequence;
            if (c->sent == c->size) {
                c->state = WCNSS_NV_WAIT;
                c->deadline = now + 5000000;
            } else
                c->deadline = now + 30000000; /* Only an accepted fragment renews the send wait. */
        }
        return c->state;
    }
    return c->state;
}
