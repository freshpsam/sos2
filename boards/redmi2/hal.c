#include "wcn_hal.h"
#include "wlan_wire.h"
#include "platform.h"
#include "wire.h"
#include "../../third_party/wcn36xx/config.h"
_Static_assert(sizeof(struct wcn36xx_hal_msg_header) == 8, "HAL wire header");

static void wipe_tx(struct wcn_hal *h) {
    volatile unsigned char *p = h->tx;
    for (unsigned i = 0; i < sizeof(h->tx); ++i) p[i] = 0;
}

static bool fail(struct wcn_hal *h, const char *why) {
    /* A failed key install would otherwise leave its key in tx until reboot;
     * nothing reads tx once the HAL has failed. */
    wipe_tx(h);
    h->failed = true;
    h->ready = false;
    h->error = why;
    return false;
}

static unsigned diagnostic_text(struct wcn_hal *h, unsigned at, const char *text) {
    while (*text && at < sizeof(h->diagnostic) - 1) h->diagnostic[at++] = *text++;
    h->diagnostic[at] = 0;
    return at;
}

static unsigned diagnostic_hex(struct wcn_hal *h, unsigned at, const char *label, unsigned value,
                               unsigned digits) {
    at = diagnostic_text(h, at, label);
    for (unsigned i = digits; i && at < sizeof(h->diagnostic) - 1; --i)
        h->diagnostic[at++] = "0123456789abcdef"[(value >> (4 * (i - 1))) & 15];
    h->diagnostic[at] = 0;
    return at;
}

static bool unexpected(struct wcn_hal *h, unsigned type, unsigned size) {
    /* Keep enough evidence on the phone to identify a new firmware message.
     * The text survives transport shutdown and later calls on this HAL. */
    unsigned at = diagnostic_text(h, 0, "HAL unexpected:");
    const char *labels[] = {" rx=", " want=", " req=", " n="};
    const unsigned values[] = {type, h->pending ? h->expected : 0xffff,
                               h->pending ? h->request : 0xffff, size};
    for (unsigned j = 0; j < 4; ++j) at = diagnostic_hex(h, at, labels[j], values[j], 4);
    const char *state = h->pending ? (h->sent ? " sent" : " queued") : " idle";
    diagnostic_text(h, at, state);
    return fail(h, h->diagnostic);
}

static bool rejected(struct wcn_hal *h, unsigned status) {
    unsigned at = diagnostic_text(h, 0, "HAL rejected: ");
    at = diagnostic_text(h, at, h->operation ? h->operation : "unknown");
    at = diagnostic_hex(h, at, " req=", h->request, 4);
    at = diagnostic_hex(h, at, " rsp=", h->expected, 4);
    at = diagnostic_hex(h, at, " status=", status, 8);
    if (h->request == WCN36XX_HAL_DOWNLOAD_NV_REQ) {
        at = diagnostic_hex(h, at, " frag=", h->fragment, 4);
        if (h->nv_restarts) diagnostic_hex(h, at, " restart=", h->nv_restarts, 1);
    }
    return fail(h, h->diagnostic);
}

/* Vendor HAL indications are asynchronous, not acknowledgements. These
 * messages need no host action for our non-HT station. Validate their wire
 * bounds without interpreting diagnostic register addresses as MMIO. */
static int informational(struct wcn_hal *h, unsigned type, unsigned size) {
    const char *error = 0;
    switch (type) {
        case WCN36XX_HAL_COEX_IND:
            if (size != sizeof(struct coex_ind_msg)) error = "malformed coexistence indication";
            break;
        case WCN36XX_HAL_OTA_TX_COMPL_IND:
            if (size < 12) error = "short TX indication";
            else if (h->tx_report_pending && !h->tx_report_disabled) {
                h->tx_report_pending = false;
                /* This is a Boolean OTA result, unlike HAL command status: 1
                 * means acknowledged. Vendor replies can append a dialog token;
                 * the legacy ABI has none, so only one report may be outstanding. */
                if (h->tx_report_generation == h->tx_generation)
                    h->tx_feedback = le32(h->rx + 8) == 1 ? WCN_TX_ACKED : WCN_TX_NO_ACK;
            }
            break;
        case WCN36XX_HAL_PRINT_REG_INFO_IND:
            /* Header, count, scenario, reason, followed by count address/value pairs. */
            if (size < 20 || (size - 20) % 8 || le32(h->rx + 8) > (size - 20) / 8)
                error = "malformed register indication";
            break;
        case WCN36XX_HAL_AVOID_FREQ_RANGE_IND:
            /* Stock carries up to 15 ranges; accept both compact and padded arrays. */
            if (size < 12 || (size - 12) % 8 || le32(h->rx + 8) > 15 ||
                le32(h->rx + 8) > (size - 12) / 8)
                error = "malformed channel-avoidance indication";
            break;
        case WCN36XX_HAL_DEL_BA_IND:
            /* Vendor's packed tHalDelBAIndMsg is 28 bytes. The imported mainline
             * declaration lacks packing and its sizeof includes host padding. */
            if (size < 28) error = "short block-ack indication";
            break;
        case WCN36XX_HAL_RSSI_NOTIFICATION_IND:
            /* The vendor extends the common four-byte RSSI flags with thresholds. */
            if (size < 12) error = "short RSSI indication";
            break;
        case WCN36XX_HAL_TX_PER_HIT_IND:
            if (size != 8) error = "malformed TX-rate indication";
            break;
        default: return 0;
    }
    if (error) {
        fail(h, error);
        return -1;
    }
    return 1;
}

static bool request(struct wcn_hal *h, unsigned type, unsigned expected, void *message,
                    unsigned size, unsigned minimum, uint64_t now) {
    if (h->pending || h->complete || size < 8 || size > sizeof(h->tx) || minimum < 12 ||
        minimum > sizeof(h->rx) || now > UINT64_MAX - 3000000)
        return fail(h, "invalid HAL request");
    put16(message, type);
    put16((unsigned char *)message + 2, 0);
    put32((unsigned char *)message + 4, size);
    memcpy(h->tx, message, size);
    h->tx_size = size;
    h->request = type;
    h->expected = expected;
    h->minimum = minimum;
    h->pending = true;
    h->sent = false;
    h->deadline = 0;
    return true;
}

/* Power save, from Linux v6.6 drivers/net/wireless/ath/wcn36xx: smd.c's
 * ENTER/EXIT_IMPS (header only) and pmc.c's handling (a refused entry leaves
 * the firmware at full power; an exit always counts as full power). */
enum { POWER_RETRY_US = 10000000 };

static void power_request(struct wcn_hal *h, const char *operation, unsigned type,
                          unsigned expected, void *message, unsigned size, uint64_t now) {
    h->operation = operation;
    if (request(h, type, expected, message, size, 12, now)) h->power_request = true;
}

static void power_leave(struct wcn_hal *h, uint64_t now) {
    struct wcn36xx_hal_exit_imps_req_msg m = {0};
    power_request(h, "radio-wake", WCN36XX_HAL_EXIT_IMPS_REQ, WCN36XX_HAL_EXIT_IMPS_RSP, &m,
                  sizeof(m), now);
}

static void power_done(struct wcn_hal *h, uint64_t now) {
    bool accepted = h->response_size >= 12 && !le32(h->rx + 8);
    h->pending = h->power_request = false;
    if (h->request == WCN36XX_HAL_ENTER_IMPS_REQ) {
        if (accepted) h->power = WCN_POWER_IDLE;
        else h->power_retry = now + POWER_RETRY_US;
    } else h->power = WCN_POWER_FULL;
}

void wcn_hal_power(struct wcn_hal *h, enum wcn_power want, uint64_t now) {
    if (!h || !h->ready || h->failed || h->pending || h->complete || want == h->power) return;
    if (h->power != WCN_POWER_FULL) {
        power_leave(h, now);
        return;
    }
    if (now < h->power_retry) return;
    struct wcn36xx_hal_enter_imps_req_msg m = {0};
    power_request(h, "radio-idle", WCN36XX_HAL_ENTER_IMPS_REQ, WCN36XX_HAL_ENTER_IMPS_RSP, &m,
                  sizeof(m), now);
}

bool wcn_hal_start(struct wcn_hal *h, struct smd_channel *channel, const void *nv, unsigned size,
                   const unsigned char mac[6]) {
    if (!h || !channel || channel->words || !nv || size < 5 || size > 1024 * 1024 || !mac ||
        (mac[0] & 1))
        return false;
    *h = (struct wcn_hal){.channel = channel,
                          .nv = (const unsigned char *)nv + 4,
                          .nv_size = size - 4,
                          .bss = 255,
                          .sta = 255,
                          .bss_sta = 255,
                          .self = 255};
    memcpy(h->mac, mac, 6);
    return true;
}

static bool status(struct wcn_hal *h) {
    /* An error can contain only the status, without the success payload. */
    if (h->response_size < 12) return fail(h, "short HAL response");
    unsigned result = h->expected == WCN36XX_HAL_START_RSP ? le16(h->rx + 8) : le32(h->rx + 8);
    if (result) return rejected(h, result);
    if (h->response_size < h->minimum) return fail(h, "short HAL response");
    return true;
}

static bool restart_nv(struct wcn_hal *h, uint64_t now) {
    /* The supplied Pronto firmware's NV handler returns 5 for a duplicate or
     * out-of-order fragment, clears the partial upload, then sends its reply.
     * A lost sequence after an accepted fragment can therefore be restarted
     * from zero. Do this once, only for a non-final NV fragment: first-fragment,
     * final parsing, other-command, malformed and transport failures stay fatal.
     * Never infer "radio absent" from the legacy host status enum here.
     * See docs/redmi2-wifi.md for the firmware evidence. */
    if (h->ready || h->phase || h->request != WCN36XX_HAL_DOWNLOAD_NV_REQ ||
        h->response_size != 12 || le32(h->rx + 8) != 5 || !h->fragment || !h->nv_sent ||
        le16(h->tx + 10) || h->nv_restarts)
        return false;
    ++h->nv_restarts;
    h->nv_sent = h->fragment = 0;
    h->pending = h->sent = h->complete = false;
    h->deadline = 0;
    h->nv_retry_at = now + 100000; /* Bounded backoff; no busy wait in the UI worker. */
    return true;
}

bool wcn_hal_poll(struct wcn_hal *h, uint64_t now) {
    if (!h || h->failed) return false;
    if (now > UINT64_MAX - 3000000) return fail(h, "wireless HAL clock overflow");
    /* Retain the acknowledged response until its owner consumes it. */
    if (h->ready && h->complete) return true;
    int result = smd_poll(h->channel, now);
    if (result < 0) return fail(h, "wireless HAL transport failed");
    if (!result) return true; /* SMD owns the opening wait; no command is sent yet. */
    /* Queueing, backpressure and the firmware reply have separate budgets.
     * Start the send wait when the worker first services this request. */
    if (h->pending && !h->deadline) h->deadline = now + 3000000;
    for (unsigned budget = 0; budget < 8; ++budget) {
        unsigned received = 0;
        result = smd_receive(h->channel, h->rx, sizeof(h->rx), &received, now);
        if (result < 0) return fail(h, "wireless HAL transport failed");
        if (!result) break;
        if (received < 8 || le32(h->rx + 4) != received || le16(h->rx + 2) > 1)
            return fail(h, "malformed HAL response");
        unsigned type = le16(h->rx);
        if (type == WCN36XX_HAL_FATAL_ERROR_IND)
            return fail(h, "wireless firmware fatal error; reboot required");
        if (type == WCN36XX_HAL_MISSED_BEACON_IND) {
            if (received != sizeof(struct wcn36xx_hal_missed_beacon_ind_msg))
                return fail(h, "short missed-beacon indication");
            if (h->rx[8] == h->bss && h->bss != 255) h->beacon_missed = true;
        } else if (type == WCN36XX_HAL_DELETE_STA_CONTEXT_IND) {
            if (received != sizeof(struct wcn36xx_hal_delete_sta_context_ind_msg))
                return fail(h, "short station-loss indication");
            const struct wcn36xx_hal_delete_sta_context_ind_msg *r = (const void *)h->rx;
            if (r->sta_id == h->sta && h->sta != 255) {
                h->link_lost = h->sta_lost = true;
                h->sta = 255;
                h->self_dpu = h->self_signature = 0;
            } else if (r->sta_id == h->bss_sta && h->bss != 255) h->link_lost = true;
        } else {
            int indication = informational(h, type, received);
            if (indication < 0) return false;
            if (indication) continue;
            if (!h->pending || !h->sent || type != h->expected)
                return unexpected(h, type, received);
            h->response_size = received;
            if (restart_nv(h, now)) break;
            if (h->power_request) {
                power_done(h, now);
                break;
            }
            if (!status(h)) return false;
            h->pending = false;
            h->complete = true;
            break;
        }
    }
    if (h->tx_report_pending && now >= h->tx_report_deadline) {
        h->tx_report_pending = false;
        h->tx_report_disabled = true;
        if (h->tx_report_generation == h->tx_generation) h->tx_feedback = WCN_TX_NO_STATUS;
        /* Do not reuse a tokenless report slot after timeout: a late report
         * could otherwise be mistaken for the next packet's acknowledgement.
         * Data/DHCP retries remain enabled; only further probing is disabled. */
    }
    /* Indications never renew the deadline, but a queued matching reply wins
     * over timeout even after a delayed worker poll. */
    if (h->pending && now >= h->deadline) return fail(h, "wireless HAL timed out");
    if (h->pending && !h->sent) {
        result = smd_send(h->channel, h->tx, h->tx_size, now);
        if (result < 0) return fail(h, "wireless HAL send failed");
        h->sent = result == 1;
        if (h->sent) h->deadline = now + 3000000;
    }
    if (h->ready) return true;
    if (h->complete) {
        h->complete = false;
        if (h->phase == 0) {
            h->nv_sent += le32(h->tx + 12);
            ++h->fragment;
            if (h->nv_sent == h->nv_size) ++h->phase;
        } else if (h->phase == 1) {
            struct wcn36xx_hal_mac_start_rsp_msg response;
            memcpy(&response, h->rx, sizeof(response));
            const struct wcnss_wlan_version *v = &response.start_rsp_params.version;
            if (v->major != 1 || v->minor < 3 || v->minor > 5 ||
                !response.start_rsp_params.stations || !response.start_rsp_params.bssids)
                return fail(h, "unsupported WCN3620 HAL firmware version");
            memcpy(h->version, v, 4);
            ++h->phase;
        } else if (h->phase == 2) {
            const struct wcn36xx_hal_add_sta_self_rsp_msg *r = (const void *)h->rx;
            if (r->self_sta_index == 255 || r->dpu_signature > 7)
                return fail(h, "invalid self station response");
            h->self = r->self_sta_index;
            h->self_dpu = r->dpu_index;
            h->self_signature = r->dpu_signature;
            h->ready = true;
            return true;
        }
    }
    if (h->pending) return true;
    if (h->phase == 0) {
        if (now < h->nv_retry_at) return true;
        h->operation = "nv";
        unsigned char packet[16 + 3072] = {0};
        unsigned n = h->nv_size - h->nv_sent;
        if (n > 3072) n = 3072;
        put16(packet + 8, h->fragment);
        put16(packet + 10, h->nv_sent + n == h->nv_size);
        put32(packet + 12, n);
        memcpy(packet + 16, h->nv + h->nv_sent, n);
        return request(h, WCN36XX_HAL_DOWNLOAD_NV_REQ, WCN36XX_HAL_DOWNLOAD_NV_RSP, packet, n + 16,
                       12, now);
    }
    if (h->phase == 1) {
        h->operation = "start";
        unsigned char packet[1024] = {0};
        unsigned n = 16;
        for (unsigned i = 0; i < sizeof(wcn36xx_cfg_vals) / sizeof(wcn36xx_cfg_vals[0]); ++i) {
            put16(packet + n, wcn36xx_cfg_vals[i].cfg_id);
            put16(packet + n + 2, 4);
            put32(packet + n + 8, wcn36xx_cfg_vals[i].value);
            n += 12;
        }
        put32(packet + 12, n - 16);
        return request(h, WCN36XX_HAL_START_REQ, WCN36XX_HAL_START_RSP, packet, n,
                       sizeof(struct wcn36xx_hal_mac_start_rsp_msg), now);
    }
    h->operation = "self";
    struct wcn36xx_hal_add_sta_self_req msg = {0};
    memcpy(msg.self_addr, h->mac, 6);
    return request(h, WCN36XX_HAL_ADD_STA_SELF_REQ, WCN36XX_HAL_ADD_STA_SELF_RSP, &msg, sizeof(msg),
                   sizeof(struct wcn36xx_hal_add_sta_self_rsp_msg), now);
}

static void station_parameters(struct wcn_hal *h, const struct wifi_station *s,
                               struct wcn36xx_hal_config_sta_params_v1 *p) {
    memcpy(p->bssid, s->bssid, 6);
    memcpy(p->mac, s->profile.mac, 6);
    p->aid = s->aid;
    p->listen_interval = 10;
    p->short_preamble_supported = (s->capabilities >> 5) & 1;
    p->sta_index = h->self;
    p->bssid_index = h->bss;
    p->encrypt_type = s->profile.security ? WCN36XX_HAL_ED_CCMP : WCN36XX_HAL_ED_NONE;
    p->rmf = s->rsn.pmf;
    p->supported_rates.op_rate_mode = STA_11bg;
    static const unsigned char dsss[] = {2, 4, 11, 22}, ofdm[] = {12, 18, 24, 36, 48, 72, 96, 108};
    /* limPopulateMatchingRateSet passes sorted, packed IE rates, preserving
     * each basic-rate bit. The HAL uses these bits for its response rates.
     * Array positions are not rate IDs; absent rates must not leave holes. */
    unsigned b = 0, a = 0;
    for (unsigned j = 0; j < sizeof(dsss); ++j)
        for (unsigned i = 0; i < s->rate_count; ++i)
            if ((s->rates[i] & 127) == dsss[j]) {
                p->supported_rates.dsss_rates[b++] = s->rates[i];
                break;
            }
    for (unsigned j = 0; j < sizeof(ofdm); ++j)
        for (unsigned i = 0; i < s->rate_count; ++i)
            if ((s->rates[i] & 127) == ofdm[j]) {
                p->supported_rates.ofdm_rates[a++] = s->rates[i];
                break;
            }
}

#define SIMPLE(type, req, rsp, body) \
    do { \
        struct type m = {0}; \
        body; \
        return request(h, req, rsp, &m, sizeof(m), 12, now) ? 0 : -1; \
    } while (0)

int wcn_hal_configure(struct wcn_hal *h, enum wifi_action action, const struct wifi_station *s,
                      uint64_t now) {
    if (!h || !s || h->failed || !h->ready || s->rate_count > sizeof(s->rates)) return -1;
    static const unsigned requests[] = {WCN36XX_HAL_INIT_SCAN_REQ,   WCN36XX_HAL_START_SCAN_REQ,
                                        WCN36XX_HAL_END_SCAN_REQ,    WCN36XX_HAL_FINISH_SCAN_REQ,
                                        WCN36XX_HAL_JOIN_REQ,        WCN36XX_HAL_CONFIG_BSS_REQ,
                                        WCN36XX_HAL_SET_LINK_ST_REQ, WCN36XX_HAL_CONFIG_BSS_REQ,
                                        WCN36XX_HAL_CONFIG_STA_REQ,  0,
                                        WCN36XX_HAL_SET_LINK_ST_REQ, WCN36XX_HAL_DELETE_STA_REQ,
                                        WCN36XX_HAL_DELETE_BSS_REQ};
    if ((unsigned)action >= sizeof(requests) / sizeof(requests[0])) return -1;
    /* Configuration runs at full power: wait out a power change, or leave
     * power save first. */
    if (h->power_request) return 0;
    if (h->power != WCN_POWER_FULL && !h->pending && !h->complete) {
        power_leave(h, now);
        return h->failed ? -1 : 0;
    }
    unsigned expected = requests[action];
    if (action == WIFI_INSTALL_KEY) {
        if (!s->install_key || (unsigned)s->install_key->type > WIFI_KEY_INTEGRITY) return -1;
        if (!h->pending && !h->complete && (h->bss == 255 || h->sta == 255)) return -1;
        expected = s->install_key->type == WIFI_KEY_PAIRWISE ? WCN36XX_HAL_SET_STAKEY_REQ
                                                             : WCN36XX_HAL_SET_BSSKEY_REQ;
    }
    if ((h->pending || h->complete) && h->request != expected) {
        fail(h, "HAL configuration changed before completion");
        return -1;
    }
    if (h->complete) {
        h->complete = false;
        if (action == WIFI_BSS_ADD || action == WIFI_BSS_UPDATE) {
            if (h->response_size < sizeof(struct wcn36xx_hal_config_bss_rsp_msg)) {
                fail(h, "short BSS response");
                return -1;
            }
            const struct wcn36xx_hal_config_bss_rsp_msg *r = (const void *)h->rx;
            if (r->bss_rsp_params.bss_index == 255 || r->bss_rsp_params.bss_sta_index == 255 ||
                r->bss_rsp_params.ucast_dpu_signature > 7) {
                fail(h, "invalid BSS indexes");
                return -1;
            }
            h->bss = r->bss_rsp_params.bss_index;
            h->bss_sta = r->bss_rsp_params.bss_sta_index;
            h->bss_dpu = r->bss_rsp_params.dpu_desc_index;
            h->bss_signature = r->bss_rsp_params.ucast_dpu_signature;
        } else if (action == WIFI_STA) {
            if (h->response_size < sizeof(struct wcn36xx_hal_config_sta_rsp_msg)) {
                fail(h, "short station response");
                return -1;
            }
            const struct wcn36xx_hal_config_sta_rsp_msg *r = (const void *)h->rx;
            if (r->params.sta_index == 255 || r->params.bssid_index != h->bss ||
                r->params.uc_ucast_sig > 7) {
                fail(h, "invalid station index or signature");
                return -1;
            }
            h->sta = h->self = r->params.sta_index;
            h->self_dpu = r->params.dpu_index;
            h->self_signature = r->params.uc_ucast_sig;
        }
        /* WDI_ProcessDelSTARsp keeps the self entry but resets its DPU to
         * the unassociated defaults, rather than retaining the old BSS key. */
        if (action == WIFI_DELETE_STA) {
            h->sta = 255;
            h->self_dpu = h->self_signature = 0;
        }
        if (action == WIFI_DELETE_BSS) {
            h->bss = h->bss_sta = 255;
            ++h->tx_generation;
            h->tx_feedback = WCN_TX_UNTESTED;
        }
        if (action == WIFI_INSTALL_KEY) wipe_tx(h);
        return 1;
    }
    if (h->pending) return 0;
    static const char *const operations[] = {
        "scan-init",  "scan-start", "scan-end", "scan-finish", "join",       "bss-add",   "link-up",
        "bss-update", "station",    "key",      "link-idle",   "delete-sta", "delete-bss"};
    h->operation = operations[action];
    switch (action) {
        case WIFI_SCAN_INIT:
            SIMPLE(wcn36xx_hal_init_scan_req_msg, WCN36XX_HAL_INIT_SCAN_REQ,
                   WCN36XX_HAL_INIT_SCAN_RSP, m.mode = HAL_SYS_MODE_SCAN);
        case WIFI_SCAN_START:
            SIMPLE(wcn36xx_hal_start_scan_req_msg, WCN36XX_HAL_START_SCAN_REQ,
                   WCN36XX_HAL_START_SCAN_RSP, m.scan_channel = s->channel);
        case WIFI_SCAN_END:
            SIMPLE(wcn36xx_hal_end_scan_req_msg, WCN36XX_HAL_END_SCAN_REQ, WCN36XX_HAL_END_SCAN_RSP,
                   m.scan_channel = s->channel);
        case WIFI_SCAN_FINISH:
            SIMPLE(wcn36xx_hal_finish_scan_req_msg, WCN36XX_HAL_FINISH_SCAN_REQ,
                   WCN36XX_HAL_FINISH_SCAN_RSP, m.mode = HAL_SYS_MODE_SCAN;
                   m.oper_channel = s->channel);
        case WIFI_JOIN:
            SIMPLE(wcn36xx_hal_join_req_msg, WCN36XX_HAL_JOIN_REQ, WCN36XX_HAL_JOIN_RSP,
                   memcpy(m.bssid, s->bssid, 6);
                   memcpy(m.self_sta_mac_addr, s->profile.mac, 6); m.channel = s->channel;
                   m.link_state = WCN36XX_HAL_LINK_PREASSOC_STATE; m.max_tx_power = 10);
        case WIFI_LINK:
            SIMPLE(wcn36xx_hal_set_link_state_req_msg, WCN36XX_HAL_SET_LINK_ST_REQ,
                   WCN36XX_HAL_SET_LINK_ST_RSP, memcpy(m.bssid, s->bssid, 6);
                   memcpy(m.self_mac_addr, s->profile.mac, 6);
                   m.state = WCN36XX_HAL_LINK_POSTASSOC_STATE);
        case WIFI_LINK_IDLE:
            SIMPLE(wcn36xx_hal_set_link_state_req_msg, WCN36XX_HAL_SET_LINK_ST_REQ,
                   WCN36XX_HAL_SET_LINK_ST_RSP, memcpy(m.bssid, s->bssid, 6);
                   memcpy(m.self_mac_addr, s->profile.mac, 6);
                   m.state = WCN36XX_HAL_LINK_IDLE_STATE);
        case WIFI_DELETE_STA:
            if (h->sta == 255) return 1;
            SIMPLE(wcn36xx_hal_delete_sta_req_msg, WCN36XX_HAL_DELETE_STA_REQ,
                   WCN36XX_HAL_DELETE_STA_RSP, m.sta_index = h->sta);
        case WIFI_DELETE_BSS:
            if (h->bss == 255) return 1;
            SIMPLE(wcn36xx_hal_delete_bss_req_msg, WCN36XX_HAL_DELETE_BSS_REQ,
                   WCN36XX_HAL_DELETE_BSS_RSP, m.bss_index = h->bss);
        case WIFI_BSS_ADD:
        case WIFI_BSS_UPDATE: {
            struct wcn36xx_hal_config_bss_req_msg_v1 m = {0};
            struct wcn36xx_hal_config_bss_params_v1 *b = &m.bss_params;
            memcpy(b->bssid, s->bssid, 6);
            memcpy(b->self_mac_addr, s->profile.mac, 6);
            b->bss_type = WCN36XX_HAL_INFRASTRUCTURE_MODE;
            b->oper_mode = 1;
            b->nw_type = WCN36XX_HAL_11G_NW_TYPE;
            b->short_slot_time_supported = (s->capabilities >> 10) & 1;
            b->beacon_interval = s->beacon_interval;
            b->dtim_period = 1;
            b->oper_channel = s->channel;
            b->rmf = s->rsn.pmf;
            b->ssid.length = s->profile.ssid_length;
            memcpy(b->ssid.ssid, s->profile.ssid, s->profile.ssid_length);
            b->action = action == WIFI_BSS_UPDATE;
            b->rateset.num_rates = s->rate_count;
            memcpy(b->rateset.rate, s->rates, s->rate_count);
            b->max_tx_power = 10;
            station_parameters(h, s, &b->sta);
            /* limAddBss / limStaSendAddBss describe the AP here, not our self
             * station. WDI_ProcessConfigBSSReq always requests a firmware index;
             * only the separate CONFIG_STA request uses ADD_STA_SELF's index. */
            b->sta.type = 1;
            memset(b->sta.mac, 0, sizeof(b->sta.mac));
            b->sta.sta_index = 255;
            b->sta.listen_interval = s->beacon_interval;
            b->sta.action = b->action;
            return request(h, WCN36XX_HAL_CONFIG_BSS_REQ, WCN36XX_HAL_CONFIG_BSS_RSP, &m,
                           sizeof(m) - WCN36XX_DIFF_BSS_PARAMS_V1_NOVHT,
                           sizeof(struct wcn36xx_hal_config_bss_rsp_msg), now)
                       ? 0
                       : -1;
        }
        case WIFI_STA: {
            struct wcn36xx_hal_config_sta_req_msg_v1 m = {0};
            station_parameters(h, s, &m.sta_params);
            return request(h, WCN36XX_HAL_CONFIG_STA_REQ, WCN36XX_HAL_CONFIG_STA_RSP, &m,
                           sizeof(m) - WCN36XX_DIFF_STA_PARAMS_V1_NOVHT,
                           sizeof(struct wcn36xx_hal_config_sta_rsp_msg), now)
                       ? 0
                       : -1;
        }
        case WIFI_INSTALL_KEY: {
            const struct wifi_key *key = s->install_key;
            struct wcn36xx_hal_keys wire = {0};
            if ((key->type == WIFI_KEY_PAIRWISE && key->index) ||
                (key->type == WIFI_KEY_GROUP && key->index > 3) ||
                (key->type == WIFI_KEY_INTEGRITY &&
                 (key->index < 4 || key->index > 5 || !s->rsn.pmf)))
                return -1;
            h->operation = key->type == WIFI_KEY_PAIRWISE ? "pairwise-key"
                           : key->type == WIFI_KEY_GROUP  ? "group-key"
                                                          : "integrity-key";
            wire.id = key->index;
            wire.unicast = key->type == WIFI_KEY_PAIRWISE;
            wire.direction = wire.unicast ? WCN36XX_HAL_TX_RX : WCN36XX_HAL_RX_ONLY;
            wire.length = 16;
            memcpy(wire.key, key->key, 16);
            memcpy(wire.rsc, key->sequence, sizeof(key->sequence));
            bool ok;
            if (wire.unicast) {
                struct wcn36xx_hal_set_sta_key_req_msg m = {0};
                m.set_sta_key_params.sta_index = h->bss_sta;
                m.set_sta_key_params.enc_type = WCN36XX_HAL_ED_CCMP;
                m.set_sta_key_params.key[0] = wire;
                m.set_sta_key_params.single_tid_rc = 1;
                ok = request(h, WCN36XX_HAL_SET_STAKEY_REQ, WCN36XX_HAL_SET_STAKEY_RSP, &m,
                             sizeof(m), 12, now);
                memset(&m, 0, sizeof(m));
            } else {
                struct wcn36xx_hal_set_bss_key_req_msg m = {0};
                m.bss_idx = h->bss;
                m.enc_type = key->type == WIFI_KEY_INTEGRITY ? WCN36XX_HAL_ED_AES_128_CMAC
                                                             : WCN36XX_HAL_ED_CCMP;
                m.num_keys = 1;
                m.keys[0] = wire;
                m.single_tid_rc = 1;
                ok = request(h, WCN36XX_HAL_SET_BSSKEY_REQ, WCN36XX_HAL_SET_BSSKEY_RSP, &m,
                             sizeof(m), 12, now);
                memset(&m, 0, sizeof(m));
            }
            memset(&wire, 0, sizeof(wire));
            return ok ? 0 : -1;
        }
    }
    return -1;
}

static struct wcn_tx_info wcn_hal_tx_info(const struct wcn_hal *h, bool data) {
    /* WDI_ProcessConfigBSSRsp stores the peer's station, DPU and signature
     * together. WDI_FillTxBd uses that complete tuple for traffic to the AP.
     * CONFIG_STA refreshes our self tuple; its signature cannot be mixed
     * with the peer's DPU, nor can the peer overwrite the self tuple. */
    return (struct wcn_tx_info){.station = data ? h->bss_sta : h->self,
                                .dpu = data ? h->bss_dpu : h->self_dpu,
                                .signature = data ? h->bss_signature : h->self_signature,
                                .data = data};
}

static bool dhcp_request(const unsigned char *p, unsigned n) {
    /* SOS2 sends non-QoS ToDS frames. Inspect the complete LLC/IP/UDP bounds,
     * and request feedback only for DHCP, not every browser packet or EAPOL. */
    if (n < 60 || (le16(p) & 0x03fc) != 0x0108 || memcmp(p + 24, "\xaa\xaa\3\0\0\0\x08\0", 8) ||
        (p[32] >> 4) != 4)
        return false;
    unsigned header = (p[32] & 15) * 4;
    if (header < 20 || header > n - 40 || p[41] != 17 || (p[38] & 0x3f) || p[39]) return false;
    const unsigned char *udp = p + 32 + header;
    return !udp[0] && udp[1] == 68 && !udp[2] && udp[3] == 67;
}

bool wcn_hal_send(struct wcn_hal *h, struct wcn_dxe *d, const void *frame, unsigned n, bool data,
                  uint64_t now) {
    if (!h || !h->ready || h->failed || !frame || n < 24 || now > UINT64_MAX - 1000000)
        return false;
    if (h->power != WCN_POWER_FULL || h->power_request) return false; /* The radio is off. */
    const unsigned char *p = frame;
    bool encrypt = p[1] & 0x40;
    struct wcn_tx_info info = wcn_hal_tx_info(h, data || encrypt);
    info.data = data;
    info.encrypt = encrypt;
    info.report = data && !h->tx_report_pending && !h->tx_report_disabled && dhcp_request(p, n);
    if (!wcn_dxe_send(d, frame, n, &info, now)) return false;
    if (info.report) {
        h->tx_report_pending = true;
        h->tx_report_deadline = now + 1000000;
        h->tx_report_generation = h->tx_generation;
        h->tx_feedback = WCN_TX_PENDING;
    }
    return true;
}

const char *wcn_hal_tx_status(const struct wcn_hal *h) {
    switch (h->tx_feedback) {
        case WCN_TX_PENDING: return "pending";
        case WCN_TX_ACKED: return "ACK";
        case WCN_TX_NO_ACK: return "no ACK";
        case WCN_TX_NO_STATUS: return "no report";
        default: return h->tx_report_disabled ? "unavailable" : 0;
    }
}
