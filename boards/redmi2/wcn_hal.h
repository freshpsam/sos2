#ifndef SOS2_WCN_HAL_H
#define SOS2_WCN_HAL_H
#include "qcom_smd.h"
#include "wifi_station.h"
#include "wcn_dxe.h"

/* Firmware power modes, as Linux's wcn36xx drives them: full power, and
 * idle-mode power save (IMPS), the radio off while not associated. */
enum wcn_power { WCN_POWER_FULL, WCN_POWER_IDLE };

struct wcn_hal {
    struct smd_channel *channel;
    const unsigned char *nv;
    unsigned nv_size, nv_sent, fragment, phase, request, response_size, minimum, expected, tx_size;
    uint64_t deadline, nv_retry_at;
    unsigned nv_restarts;
    bool pending, sent, complete, failed, ready, link_lost, sta_lost, beacon_missed;
    const char *error, *operation;
    char diagnostic[112];
    unsigned char tx[4096], rx[4096], mac[6];
    unsigned char bss, sta, bss_sta, bss_dpu, bss_signature, self, self_dpu, self_signature;
    enum wcn_power power;
    bool power_request;
    uint64_t power_retry;
};

bool wcn_hal_start(struct wcn_hal *, struct smd_channel *, const void *, unsigned,
                   const unsigned char mac[6]);
bool wcn_hal_poll(struct wcn_hal *, uint64_t);
int wcn_hal_configure(struct wcn_hal *, enum wifi_action, const struct wifi_station *, uint64_t);
bool wcn_hal_send(struct wcn_hal *, struct wcn_dxe *, const void *, unsigned, bool, uint64_t);
/* Moves the firmware toward `want` between other requests. A configuration
 * request first returns it to full power, and nothing is sent with the radio
 * off. A refused entry is retried after 10 s. */
void wcn_hal_power(struct wcn_hal *, enum wcn_power want, uint64_t now);
#endif
