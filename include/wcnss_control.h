#ifndef SOS2_WCNSS_CONTROL_H
#define SOS2_WCNSS_CONTROL_H
#include "qcom_smd.h"

enum wcnss_state {
    WCNSS_FAILED,
    WCNSS_VERSION_SEND,
    WCNSS_VERSION_WAIT,
    WCNSS_NV_SEND,
    WCNSS_NV_WAIT,
    WCNSS_BOOT_WAIT,
    WCNSS_READY
};

/* WCNSS_CTRL initialization only. READY means firmware accepted board-specific
 * NV data, not that the radio is associated or has an Ethernet/IP connection. */
struct wcnss_control {
    struct smd_channel *channel;
    const char *error;
    const unsigned char *nv;
    unsigned size, sent, sequence;
    enum wcnss_state state;
    uint64_t deadline;
    bool cold_boot_done;
    unsigned char version[4], packet[16 + 3072];
};

/* nv is the original Redmi 2 board file, including its four-byte validity bitmap. */
bool wcnss_control_start(struct wcnss_control *, struct smd_channel *, const void *nv,
                         unsigned size, uint64_t now);
/* Up to eight receives or one send per tick; no sleeping or busy waiting. */
enum wcnss_state wcnss_control_poll(struct wcnss_control *, uint64_t now);
#endif
