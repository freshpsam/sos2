#include "utils/includes.h"
#include "utils/common.h"
#include "common/sae.h"
#include "common/wpa_common.h"
#include "common/ieee802_11_defs.h"
#include "rsn_supp/wpa.h"
#include "crypto/aes_wrap.h"
#include "crypto/sha1.h"
#include "wifi_station.h"
#include "wifi/port.h"

enum { KEY_QUEUE = 4, TX_QUEUE = 4, AUTH_FRAME = 1536 };

bool wifi_auth_credential(struct wifi_profile *p, const unsigned char *password, unsigned n) {
    if (!p || p->security > WIFI_WPA3_SAE || p->ssid_length > 32 || !p->ssid_length) return false;
    wifi_secret_clear(p->pmk, sizeof(p->pmk));
    wifi_secret_clear(p->password, sizeof(p->password));
    p->password_length = 0;
    if (p->security == WIFI_OPEN) return !n;
    if (!password || n < (p->security == WIFI_WPA2_PSK ? 8u : 1u) || n > 63 ||
        memchr(password, 0, n))
        return false;
    /* Both keep the password, so the network's settings page can show it;
     * WPA2 authenticates with its PMK. */
    memcpy(p->password, password, n);
    p->password_length = n;
    if (p->security == WIFI_WPA3_SAE) return true;
    char phrase[64] = {0};
    memcpy(phrase, password, n);
    int result = pbkdf2_sha1(phrase, p->ssid, p->ssid_length, 4096, p->pmk, 32);
    wifi_secret_clear(phrase, sizeof(phrase));
    if (result) {
        wifi_secret_clear(p->pmk, sizeof(p->pmk));
        wifi_secret_clear(p->password, sizeof(p->password));
        p->password_length = 0;
    }
    return !result;
}

struct saved_key {
    struct wifi_key key;
    bool valid;
};

struct wifi_auth {
    struct wpa_sm *wpa;
    struct sae_data sae;
    struct wpabuf *token;
    enum wpa_states state;
    struct wifi_rsn rsn;
    unsigned security;
    unsigned char mac[6], bssid[6], sae_frame[512];
    unsigned sae_length, token_requests;
    struct wifi_key keys[KEY_QUEUE];
    uint64_t key_order[KEY_QUEUE], next_order;
    unsigned key_head, key_count, tx_head, tx_count;

    struct {
        uint64_t order;
        unsigned length;
        unsigned char data[AUTH_FRAME];
    } tx[TX_QUEUE];
    struct saved_key installed[7];
    bool failed, authorized, associated, completion;
};

static int akm(unsigned security) {
    return security == WIFI_WPA3_SAE ? WPA_KEY_MGMT_SAE : WPA_KEY_MGMT_PSK;
}

bool wifi_rsn_select(struct wifi_rsn *out, unsigned security, const unsigned char *ie, unsigned n,
                     const unsigned char *ext, unsigned en) {
    struct wpa_ie_data parsed;
    if (!out || security < WIFI_WPA2_PSK || security > WIFI_WPA3_SAE || !ie || n < 2 ||
        n > sizeof(out->ie) || en > sizeof(out->extension) || (en && !ext) ||
        wpa_parse_wpa_ie_rsn(ie, n, &parsed))
        return false;
    if (parsed.group_cipher != WPA_CIPHER_CCMP || !(parsed.pairwise_cipher & WPA_CIPHER_CCMP) ||
        !(parsed.key_mgmt & akm(security)))
        return false;
    bool pmf = parsed.capabilities & WPA_CAPABILITY_MFPC;
    if ((parsed.capabilities & WPA_CAPABILITY_MFPR) && !pmf) return false;
    if (security == WIFI_WPA3_SAE && !pmf) return false;
    if (pmf && parsed.mgmt_group_cipher != WPA_CIPHER_AES_128_CMAC) return false;
    if (en && (en < 3 || ext[0] != WLAN_EID_RSNX || ext[1] != en - 2 ||
               (unsigned)(ext[2] & 15) + 1 != en - 2))
        return false;
    *out = (struct wifi_rsn){.length = n,
                             .extension_length = en,
                             .pmf = pmf,
                             .h2e = security == WIFI_WPA3_SAE && en &&
                                    (ext[2] & BIT(WLAN_RSNX_CAPAB_SAE_H2E))};
    memcpy(out->ie, ie, n);
    if (en) memcpy(out->extension, ext, en);
    return true;
}

bool wifi_rsn_response(const struct wifi_rsn *original, unsigned security, const unsigned char *ie,
                       unsigned n) {
    struct wifi_rsn parsed;
    return wifi_rsn_select(&parsed, security, ie, n, 0, 0) && parsed.pmf == original->pmf;
}

static void state_changed(void *p, enum wpa_states state) {
    struct wifi_auth *a = p;
    a->state = state;
    if (state == WPA_COMPLETED) a->completion = true;
}

static enum wpa_states get_state(void *p) { return ((struct wifi_auth *)p)->state; }

static void disconnected(void *p, u16 reason) {
    (void)reason;
    ((struct wifi_auth *)p)->failed = true;
}

static void reconnect(void *p) { ((struct wifi_auth *)p)->failed = true; }

static void *network_context(void *p) { return p; }

static int get_bssid(void *p, u8 *out) {
    memcpy(out, ((struct wifi_auth *)p)->bssid, 6);
    return 0;
}

static void cancel_timeout(void *p) { (void)p; }

static int beacon_ie(void *p) {
    (void)p;
    return 0;
}

static int set_protection(void *p, const u8 *addr, int protection, int kind) {
    (void)p;
    (void)addr;
    (void)protection;
    (void)kind;
    return 0;
}

static int add_pmkid(void *p, void *network, const u8 *bssid, const u8 *pmkid, const u8 *cache,
                     const u8 *pmk, size_t length, u32 lifetime, u8 threshold, int key_mgmt) {
    (void)p;
    (void)network;
    (void)bssid;
    (void)pmkid;
    (void)cache;
    (void)pmk;
    (void)length;
    (void)lifetime;
    (void)threshold;
    (void)key_mgmt;
    return 0;
}

static int remove_pmkid(void *p, void *network, const u8 *bssid, const u8 *pmkid, const u8 *cache) {
    (void)p;
    (void)network;
    (void)bssid;
    (void)pmkid;
    (void)cache;
    return 0;
}

static unsigned key_slot(const struct wifi_key *key) {
    return key->type == WIFI_KEY_PAIRWISE ? 0
           : key->type == WIFI_KEY_GROUP  ? 1 + key->index
                                          : key->index + 1;
}

static int set_key(void *p, int link_id, enum wpa_alg algorithm, const u8 *address, int index,
                   int tx, const u8 *sequence, size_t sequence_length, const u8 *key, size_t length,
                   enum key_flag flags) {
    struct wifi_auth *a = p;
    (void)tx;
    /* This HAL cannot stage a next RX-only PTK and later activate it without
     * reinstalling. hostap explicitly permits rejecting this optimization.
     * Treating KEY_FLAG_NEXT as TX_RX enables encryption before initial M4. */
    if (flags & (KEY_FLAG_NEXT | KEY_FLAG_MODIFY)) return -1;
    if (algorithm == WPA_ALG_NONE) return 0; /* Destruction clears the entire BSS. */
    if (link_id != -1 || !key || length != 16 || sequence_length > 8 ||
        (!sequence && sequence_length) || a->key_count == KEY_QUEUE)
        return -1;
    struct wifi_key candidate = {.index = (unsigned)index};
    if (algorithm == WPA_ALG_CCMP && (flags & KEY_FLAG_PAIRWISE) &&
        (flags & KEY_FLAG_RX_TX) == KEY_FLAG_RX_TX && index == 0 && address &&
        !memcmp(address, a->bssid, 6))
        candidate.type = WIFI_KEY_PAIRWISE;
    else if (algorithm == WPA_ALG_CCMP && (flags & KEY_FLAG_GROUP) && index >= 0 && index <= 3)
        candidate.type = WIFI_KEY_GROUP;
    else if (algorithm == WPA_ALG_BIP_CMAC_128 && a->rsn.pmf && (flags & KEY_FLAG_GROUP) &&
             index >= 4 && index <= 5)
        candidate.type = WIFI_KEY_INTEGRITY;
    else return -1;
    memcpy(candidate.key, key, 16);
    if (sequence_length) memcpy(candidate.sequence, sequence, sequence_length);
    struct saved_key *old = &a->installed[key_slot(&candidate)];
    /* Never reset a replay counter for an already installed key, even if a
     * caller or retransmitted EAPOL message requests that key again. */
    if (old->valid && !os_memcmp_const(old->key.key, key, 16)) {
        wifi_crypto_wipe(&candidate, sizeof(candidate));
        return 0;
    }
    for (unsigned i = 0; i < a->key_count; ++i) {
        const struct wifi_key *pending = &a->keys[(a->key_head + i) % KEY_QUEUE];
        if (key_slot(pending) == key_slot(&candidate) && !os_memcmp_const(pending->key, key, 16)) {
            wifi_crypto_wipe(&candidate, sizeof(candidate));
            return 0;
        }
    }
    if (a->next_order == UINT64_MAX) {
        wifi_crypto_wipe(&candidate, sizeof(candidate));
        return -1;
    }
    unsigned at = (a->key_head + a->key_count++) % KEY_QUEUE;
    a->keys[at] = candidate;
    a->key_order[at] = a->next_order++;
    return 0;
}

static int send_eapol(void *p, const u8 *destination, u16 protocol, const u8 *data, size_t n) {
    struct wifi_auth *a = p;
    if (protocol != 0x888e || memcmp(destination, a->bssid, 6) || n < 4 || n > AUTH_FRAME ||
        a->tx_count == TX_QUEUE || a->next_order == UINT64_MAX)
        return -1;
    unsigned at = (a->tx_head + a->tx_count++) % TX_QUEUE;
    a->tx[at].order = a->next_order++;
    a->tx[at].length = n;
    memcpy(a->tx[at].data, data, n);
    return 0;
}

static u8 *alloc_eapol(void *p, u8 type, const void *data, u16 n, size_t *total, void **body) {
    (void)p;
    if (n > AUTH_FRAME - 4) return 0;
    u8 *frame = os_zalloc(n + 4);
    if (!frame) return 0;
    frame[0] = 2;
    frame[1] = type;
    WPA_PUT_BE16(frame + 2, n);
    if (data) memcpy(frame + 4, data, n);
    *total = n + 4;
    *body = frame + 4;
    return frame;
}

static bool build_commit(struct wifi_auth *a) {
    struct wpabuf *b = wpabuf_alloc(sizeof(a->sae_frame));
    if (!b) return false;
    wpabuf_put_le16(b, WLAN_AUTH_SAE);
    wpabuf_put_le16(b, 1);
    wpabuf_put_le16(b, a->rsn.h2e ? WLAN_STATUS_SAE_HASH_TO_ELEMENT : 0);
    bool ok = !sae_write_commit(&a->sae, b, a->token, 0, 0);
    if (ok) {
        a->sae_length = wpabuf_len(b);
        memcpy(a->sae_frame, wpabuf_head(b), a->sae_length);
        a->sae.state = SAE_COMMITTED;
    }
    wpabuf_free(b);
    return ok;
}

struct wifi_auth *wifi_auth_create(const struct wifi_profile *p, const unsigned char bssid[6],
                                   const struct wifi_rsn *rsn,
                                   int (*random)(void *, unsigned char *, size_t), void *context,
                                   uint64_t now) {
    if (!p || !bssid || !rsn || !random || p->security < WIFI_WPA2_PSK ||
        p->security > WIFI_WPA3_SAE)
        return 0;
    wifi_port_bind(random, context, now);
    struct wifi_auth *a = os_zalloc(sizeof(*a));
    if (!a) return 0;
    struct wpa_sm_ctx *cb = os_zalloc(sizeof(*cb));
    if (!cb) {
        os_free(a);
        return 0;
    }
    *cb = (struct wpa_sm_ctx){.ctx = a,
                              .msg_ctx = a,
                              .set_state = state_changed,
                              .get_state = get_state,
                              .deauthenticate = disconnected,
                              .reconnect = reconnect,
                              .set_key = set_key,
                              .get_network_ctx = network_context,
                              .get_bssid = get_bssid,
                              .ether_send = send_eapol,
                              .get_beacon_ie = beacon_ie,
                              .cancel_auth_timeout = cancel_timeout,
                              .alloc_eapol = alloc_eapol,
                              .add_pmkid = add_pmkid,
                              .remove_pmkid = remove_pmkid,
                              .mlme_setprotection = set_protection};
    a->security = p->security;
    a->rsn = *rsn;
    memcpy(a->mac, p->mac, 6);
    memcpy(a->bssid, bssid, 6);
    a->wpa = wpa_sm_init(cb);
    if (!a->wpa) {
        os_free(cb);
        os_free(a);
        return 0;
    }
    struct rsn_supp_config config = {
        .network_ctx = a, .allowed_pairwise_cipher = WPA_CIPHER_CCMP, .eap_workaround = 0};
    wpa_sm_set_config(a->wpa, &config);
    wpa_sm_set_own_addr(a->wpa, p->mac);
    wpa_sm_set_ssid(a->wpa, p->ssid, p->ssid_length);
    wpa_sm_set_param(a->wpa, WPA_PARAM_PROTO, WPA_PROTO_RSN);
    wpa_sm_set_param(a->wpa, WPA_PARAM_PAIRWISE, WPA_CIPHER_CCMP);
    wpa_sm_set_param(a->wpa, WPA_PARAM_GROUP, WPA_CIPHER_CCMP);
    wpa_sm_set_param(a->wpa, WPA_PARAM_KEY_MGMT, akm(p->security));
    wpa_sm_set_param(a->wpa, WPA_PARAM_MGMT_GROUP, rsn->pmf ? WPA_CIPHER_AES_128_CMAC : 0);
    wpa_sm_set_param(a->wpa, WPA_PARAM_MFP,
                     rsn->pmf ? MGMT_FRAME_PROTECTION_REQUIRED : NO_MGMT_FRAME_PROTECTION);
    wpa_sm_set_param(a->wpa, WPA_PARAM_RSN_ENABLED, 1);
    wpa_sm_set_param(a->wpa, WPA_PARAM_SAE_PWE, rsn->h2e ? 1 : 0);
    /* PTK0 replacement is recovered by a fresh association, after draining
     * old DMA traffic and deleting the old firmware BSS and keys. */
    wpa_sm_set_param(a->wpa, WPA_PARAM_DENY_PTK0_REKEY, 1);
    if (wpa_sm_set_ap_rsn_ie(a->wpa, rsn->ie, rsn->length) ||
        wpa_sm_set_ap_rsnxe(a->wpa, rsn->extension, rsn->extension_length))
        goto fail;
    if (p->security == WIFI_WPA2_PSK) wpa_sm_set_pmk(a->wpa, p->pmk, 32, 0, 0);
    else {
        if (!p->password_length || p->password_length > 63 || sae_set_group(&a->sae, 19)) goto fail;
        a->sae.akmp = WPA_KEY_MGMT_SAE;
        if (rsn->h2e) {
            const int groups[] = {19, 0};
            struct sae_pt *pt = sae_derive_pt(groups, p->ssid, p->ssid_length, p->password,
                                              p->password_length, 0, 0);
            if (!pt) goto fail;
            int result = sae_prepare_commit_pt(&a->sae, pt, p->mac, bssid, 0, 0);
            sae_deinit_pt(pt);
            if (result) goto fail;
        } else if (sae_prepare_commit(p->mac, bssid, p->password, p->password_length, &a->sae))
            goto fail;
        if (!build_commit(a)) goto fail;
    }
    if (wifi_port_failed()) goto fail;
    return a;
fail:
    wifi_auth_destroy(a);
    return 0;
}

void wifi_auth_destroy(struct wifi_auth *a) {
    if (a) {
        wpa_sm_deinit(a->wpa);
        sae_clear_data(&a->sae);
        wpabuf_clear_free(a->token);
        wifi_crypto_wipe(a, sizeof(*a));
        os_free(a);
    }
}

bool wifi_auth_failed(const struct wifi_auth *a) { return !a || a->failed || wifi_port_failed(); }

bool wifi_auth_pairwise(const struct wifi_auth *a) { return a && a->installed[0].valid; }

bool wifi_auth_has_key(const struct wifi_auth *a, enum wifi_key_type type, unsigned index) {
    if (!a || (unsigned)type > WIFI_KEY_INTEGRITY || (type == WIFI_KEY_PAIRWISE && index) ||
        (type == WIFI_KEY_GROUP && index > 3) ||
        (type == WIFI_KEY_INTEGRITY && (index < 4 || index > 5)))
        return false;
    struct wifi_key key = {.type = type, .index = index};
    return a->installed[key_slot(&key)].valid;
}

bool wifi_auth_ready(const struct wifi_auth *a) {
    return a && a->authorized && !wifi_auth_failed(a);
}

bool wifi_auth_busy(const struct wifi_auth *a) { return a && (a->key_count || a->tx_count); }

void wifi_auth_poll(struct wifi_auth *a, uint64_t now) {
    if (!a) return;
    wifi_port_tick(now);
    bool group = false, integrity = !a->rsn.pmf;
    for (unsigned i = 1; i < 5; ++i) group |= a->installed[i].valid;
    for (unsigned i = 5; i < 7; ++i) integrity |= a->installed[i].valid;
    if (a->completion && !a->key_count && !a->tx_count && a->installed[0].valid && group &&
        integrity)
        a->authorized = true;
}

unsigned wifi_auth_association_ie(struct wifi_auth *a, unsigned char *out, unsigned capacity) {
    if (!a || capacity < 96) return 0;
    size_t n = capacity;
    if (wpa_sm_set_assoc_wpa_ie_default(a->wpa, out, &n)) return 0;
    size_t en = capacity - n;
    if (wpa_sm_set_assoc_rsnxe_default(a->wpa, out + n, &en)) return 0;
    return n + en;
}

unsigned wifi_auth_sae_tx(struct wifi_auth *a, unsigned char *out, unsigned capacity) {
    if (!a || a->sae_length > capacity) return 0;
    memcpy(out, a->sae_frame, a->sae_length);
    return a->sae_length;
}

int wifi_auth_sae_rx(struct wifi_auth *a, const unsigned char *p, unsigned n) {
    if (!a || a->security != WIFI_WPA3_SAE || n < 6 || n > 512 || WPA_GET_LE16(p) != WLAN_AUTH_SAE)
        return 0;
    unsigned transaction = WPA_GET_LE16(p + 2), status = WPA_GET_LE16(p + 4);
    p += 6;
    n -= 6;
    if (transaction == 1 && a->sae.state == SAE_COMMITTED) {
        if (status == WLAN_STATUS_ANTI_CLOGGING_TOKEN_REQ) {
            if (n < 3 || WPA_GET_LE16(p) != 19 || ++a->token_requests > 3) return -1;
            p += 2;
            n -= 2;
            if (a->rsn.h2e) {
                if (n < 4 || p[0] != WLAN_EID_EXTENSION || p[1] != n - 2 ||
                    p[2] != WLAN_EID_EXT_ANTI_CLOGGING_TOKEN)
                    return -1;
                p += 3;
                n -= 3;
            }
            if (n > 128) return -1;
            wpabuf_clear_free(a->token);
            a->token = wpabuf_alloc_copy(p, n);
            return a->token && build_commit(a) ? 1 : -1;
        }
        if (status != (a->rsn.h2e ? WLAN_STATUS_SAE_HASH_TO_ELEMENT : 0)) return -1;
        int groups[] = {19, 0};
        u16 result = sae_parse_commit(&a->sae, p, n, 0, 0, groups, a->rsn.h2e, 0);
        if (result == SAE_SILENTLY_DISCARD) return 0;
        if (result || sae_process_commit(&a->sae)) return -1;
        /* No group negotiation was attempted, so any peer rejection list is
         * inconsistent with this exchange (including the 2024 downgrade case). */
        if (a->sae.tmp->peer_rejected_groups && wpabuf_len(a->sae.tmp->peer_rejected_groups))
            return -1;
        struct wpabuf *b = wpabuf_alloc(sizeof(a->sae_frame));
        if (!b) return -1;
        wpabuf_put_le16(b, WLAN_AUTH_SAE);
        wpabuf_put_le16(b, 2);
        wpabuf_put_le16(b, 0);
        result = sae_write_confirm(&a->sae, b);
        if (!result) {
            a->sae_length = wpabuf_len(b);
            memcpy(a->sae_frame, wpabuf_head(b), a->sae_length);
            a->sae.state = SAE_CONFIRMED;
        }
        wpabuf_free(b);
        return result ? -1 : 1;
    }
    if (transaction == 2 && a->sae.state == SAE_CONFIRMED) {
        /* Send-Confirm is authenticated by the verifier. Peers in the Accepted
         * state (e.g. hostapd re-authenticating a station) send 0xffff; the
         * "discard 0xffff" rule applies only to our own Accepted state. */
        if (status || n != 34 || sae_check_confirm(&a->sae, p, n, 0)) return -1;
        a->sae.state = SAE_ACCEPTED;
        wpa_sm_set_pmk(a->wpa, a->sae.pmk, a->sae.pmk_len, a->sae.pmkid, a->bssid);
        sae_clear_temp_data(&a->sae);
        return 2;
    }
    return 0;
}

bool wifi_auth_associated(struct wifi_auth *a) {
    if (!a || wifi_auth_failed(a) || (a->security == WIFI_WPA3_SAE && a->sae.state != SAE_ACCEPTED))
        return false;
    a->associated = true;
    a->state = WPA_ASSOCIATED;
    wpa_sm_notify_assoc(a->wpa, a->bssid);
    return true;
}

void wifi_auth_eapol(struct wifi_auth *a, const unsigned char *p, unsigned n,
                     bool protected_frame) {
    if (!a || !a->associated || a->failed || n < 4 || n > AUTH_FRAME ||
        WPA_GET_BE16(p + 2) != n - 4)
        return;
    wpa_sm_rx_eapol(a->wpa, a->bssid, p, n,
                    protected_frame ? FRAME_ENCRYPTED : FRAME_NOT_ENCRYPTED);
}

/* Preserve hostap callback order across the asynchronous HAL and DMA paths.
 * Initial M4 precedes PTK installation; group rekey installs the GTK before
 * its encrypted response. Independent key/TX queues must not reorder them. */
const struct wifi_key *wifi_auth_next_key(struct wifi_auth *a) {
    if (!a || !a->key_count || (a->tx_count && a->tx[a->tx_head].order < a->key_order[a->key_head]))
        return 0;
    return &a->keys[a->key_head];
}

void wifi_auth_key_done(struct wifi_auth *a) {
    if (!wifi_auth_next_key(a)) return;
    struct wifi_key *key = &a->keys[a->key_head];
    a->installed[key_slot(key)] = (struct saved_key){.key = *key, .valid = true};
    wifi_crypto_wipe(key, sizeof(*key));
    a->key_head = (a->key_head + 1) % KEY_QUEUE;
    --a->key_count;
}

const unsigned char *wifi_auth_next_tx(struct wifi_auth *a, unsigned *n) {
    if (!a || !a->tx_count || (a->key_count && a->key_order[a->key_head] < a->tx[a->tx_head].order))
        return 0;
    *n = a->tx[a->tx_head].length;
    return a->tx[a->tx_head].data;
}

void wifi_auth_tx_done(struct wifi_auth *a) {
    unsigned n;
    if (wifi_auth_next_tx(a, &n)) {
        wifi_crypto_wipe(&a->tx[a->tx_head], sizeof(a->tx[0]));
        a->tx_head = (a->tx_head + 1) % TX_QUEUE;
        --a->tx_count;
    }
}

bool wifi_auth_verify_group_management(struct wifi_auth *a, const unsigned char *frame,
                                       unsigned n) {
    /* BIP-CMAC-128, IEEE 802.11w MMIE. Firmware may leave it intact. */
    if (!a || !a->authorized || !a->rsn.pmf || n < 44 || n > 512) return false;
    const unsigned char *mmie = frame + n - 18;
    if (mmie[0] != 76 || mmie[1] != 16) return false;
    unsigned index = WPA_GET_LE16(mmie + 2);
    if (index < 4 || index > 5) return false;
    struct saved_key *key = &a->installed[index + 1];
    if (!key->valid) return false;
    int cmp = 0;
    for (int i = 5; i >= 0; --i)
        if (mmie[4 + i] != key->key.sequence[i]) {
            cmp = mmie[4 + i] > key->key.sequence[i] ? 1 : -1;
            break;
        }
    if (cmp <= 0) return false;
    unsigned char aad[20], body[512], mic[16];
    aad[0] = frame[0];
    aad[1] = frame[1] & ~0x38;
    memcpy(aad + 2, frame + 4, 18);
    memcpy(body, frame + 24, n - 24);
    memset(body + n - 24 - 8, 0, 8);
    const u8 *parts[] = {aad, body};
    size_t lengths[] = {20, n - 24};
    bool ok = !omac1_aes_128_vector(key->key.key, 2, parts, lengths, mic) &&
              !os_memcmp_const(mic, mmie + 10, 8);
    if (ok) memcpy(key->key.sequence, mmie + 4, 6);
    wifi_crypto_wipe(body, sizeof(body));
    wifi_crypto_wipe(mic, sizeof(mic));
    return ok;
}
