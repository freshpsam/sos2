#include "wifi_station.h"
#include "platform.h"
#include "wire.h"
static const unsigned char broadcast[6] = {255, 255, 255, 255, 255, 255};
static const unsigned char snap[6] = {0xaa, 0xaa, 3, 0, 0, 0};
static const unsigned char supported[] = {2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108};

static bool unicast(const unsigned char *p) { return !(p[0] & 1) && memcmp(p, "\0\0\0\0\0\0", 6); }

static uint64_t fresh_time(struct wifi_station *s, uint64_t now) {
    return s->io.clock ? s->io.clock(s->io.context) : now;
}

void wifi_station_stop(struct wifi_station *s, const char *why) {
    if (s) {
        wifi_auth_destroy(s->auth);
        s->auth = 0;
        s->state = WIFI_FAILED;
        s->error = why;
    }
}

static void enter(struct wifi_station *s, enum wifi_state state, uint64_t now) {
    s->state = state;
    s->deadline = now + 3000000;
    s->sent = false;
    s->attempts = 0;
}

void wifi_station_reconnect(struct wifi_station *s, const char *why, uint64_t now) {
    if (!s || s->state == WIFI_FAILED || s->state == WIFI_OFF || s->state == WIFI_RECOVER ||
        s->state == WIFI_BACKOFF)
        return;
    s->error = why;
    s->query_active = false;
    enter(s, WIFI_RECOVER, now);
    s->deadline = now + 6000000;
}

static bool start_station(struct wifi_station *s, const struct wifi_profile *p,
                          const struct wifi_ops *io, uint64_t now, bool survey) {
    if (!s || !p || !io || !io->configure || !io->send || !io->receive || !unicast(p->mac) ||
        (!p->ssid_length && !survey) || p->ssid_length > 32 || p->security > WIFI_WPA3_SAE ||
        p->channel > 11 || now > UINT64_MAX - 60000000)
        return false;
    if (p->security && (!io->random || !io->drained)) return false;
    if (p->security == WIFI_WPA3_SAE && (!p->password_length || p->password_length > 63))
        return false;
    *s = (struct wifi_station){
        .profile = *p, .io = *io, .channel = p->channel ? p->channel : 1, .survey = survey};
    enter(s, WIFI_SCAN_SETUP, now);
    return true;
}

bool wifi_station_start(struct wifi_station *s, const struct wifi_profile *p,
                        const struct wifi_ops *io, uint64_t now) {
    return start_station(s, p, io, now, false);
}

bool wifi_station_survey(struct wifi_station *s, const unsigned char mac[6],
                         const struct wifi_ops *io, uint64_t now) {
    if (!mac) return false;
    struct wifi_profile p = {0};
    memcpy(p.mac, mac, 6);
    return start_station(s, &p, io, now, true);
}

static void header(struct wifi_station *s, unsigned char *p, unsigned type) {
    memset(p, 0, 24);
    put16(p, type);
    memcpy(p + 4, s->bssid, 6);
    memcpy(p + 10, s->profile.mac, 6);
    memcpy(p + 16, s->bssid, 6);
    put16(p + 22, (s->sequence++ & 4095) << 4);
}

static unsigned network_elements(const struct wifi_station *s, unsigned char *frame, unsigned n) {
    frame[n++] = 0;
    frame[n++] = s->profile.ssid_length;
    memcpy(frame + n, s->profile.ssid, s->profile.ssid_length);
    n += s->profile.ssid_length;
    frame[n++] = 1;
    unsigned first = s->rate_count > 8 ? 8 : s->rate_count;
    frame[n++] = first;
    memcpy(frame + n, s->rates, first);
    n += first;
    if (s->rate_count > 8) {
        frame[n++] = 50;
        frame[n++] = s->rate_count - 8;
        memcpy(frame + n, s->rates + 8, s->rate_count - 8);
        n += s->rate_count - 8;
    }
    return n;
}

static void heard_ap(struct wifi_station *s, uint64_t now) {
    s->last_rx = now;
    s->probe_deadline = s->probe_next = 0;
    s->probe_attempts = 0;
}

void wifi_station_beacon_missed(struct wifi_station *s, uint64_t now) {
    if (!s || s->state != WIFI_ONLINE || s->probe_deadline || now > UINT64_MAX - 3000000) return;
    /* Stock probes on heartbeat loss before tearing down the association.
     * Firmware may filter beacons, so a host beacon gap alone is not loss. */
    s->probe_deadline = now + 3000000;
    s->probe_next = now;
    s->probe_attempts = 0;
}

void wifi_station_standby(struct wifi_station *s, bool standby, uint64_t now) {
    if (!s) return;
    s->standby = standby;
    if (!standby || s->survey || s->state == WIFI_IDLE || s->state == WIFI_RECOVER) return;
    enum wifi_state before = s->state;
    wifi_station_reconnect(s, 0, now);
    if (before != s->state) s->leaving = true;
}

static void monitor_link(struct wifi_station *s, uint64_t now) {
    if (now - s->last_rx >= 10000000) wifi_station_beacon_missed(s, now);
    if (!s->probe_deadline) return;
    if (now >= s->probe_deadline) {
        wifi_station_reconnect(s, "access point did not answer probes", now);
        return;
    }
    if (now < s->probe_next || s->probe_attempts >= 3) return;
    unsigned char frame[80];
    header(s, frame, 0x40);
    unsigned n = network_elements(s, frame, 24);
    if (s->io.send(s->io.context, frame, n, false)) {
        ++s->probe_attempts;
        s->probe_next = now + 1000000;
    }
}

static bool management(struct wifi_station *s) {
    unsigned char frame[640];
    unsigned n;
    if (s->state == WIFI_AUTH) {
        header(s, frame, 0xb0);
        if (s->profile.security == WIFI_WPA3_SAE) {
            n = wifi_auth_sae_tx(s->auth, frame + 24, sizeof(frame) - 24);
            if (!n) return false;
            n += 24;
        } else {
            put16(frame + 24, 0);
            put16(frame + 26, 1);
            put16(frame + 28, 0);
            n = 30;
        }
    } else {
        header(s, frame, 0);
        put16(frame + 24,
              1 | (s->profile.security ? 16 : 0) | (s->capabilities & (1u << 5 | 1u << 10)));
        put16(frame + 26, 10);
        n = 28;
        n = network_elements(s, frame, n);
        if (s->auth) {
            unsigned count = wifi_auth_association_ie(s->auth, frame + n, sizeof(frame) - n);
            if (!count) return false;
            n += count;
        }
    }
    return s->io.send(s->io.context, frame, n, false);
}

static int command(struct wifi_station *s, enum wifi_action action) {
    if (s->configuring && s->pending_action != action) {
        wifi_station_stop(s, "wireless control overlap");
        return -1;
    }
    s->configuring = true;
    s->pending_action = action;
    int result = s->io.configure(s->io.context, action, s);
    if (result < 0) wifi_station_stop(s, "wireless firmware configuration failed");
    else if (result == 1) {
        s->configuring = false;
        if (action == WIFI_SCAN_INIT) s->scan_session = true;
        if (action == WIFI_SCAN_FINISH) s->scan_session = false;
        if (action == WIFI_SCAN_START) s->scan_active = true;
        if (action == WIFI_SCAN_END) s->scan_active = false;
        if (action == WIFI_JOIN) s->joined = true;
        if (action == WIFI_BSS_ADD) s->bss_live = true;
        if (action == WIFI_STA) s->sta_live = true;
        if (action == WIFI_LINK_IDLE) s->joined = false;
        if (action == WIFI_DELETE_STA) s->sta_live = false;
        if (action == WIFI_DELETE_BSS) s->bss_live = false;
        if (action == WIFI_INSTALL_KEY) {
            wifi_auth_key_done(s->auth);
            s->install_key = 0;
        }
    }
    return result;
}

static int configure(struct wifi_station *s, enum wifi_action action, enum wifi_state next,
                     uint64_t now) {
    int result = command(s, action);
    if (result == 1) enter(s, next, now);
    return result;
}

static void recover_command(struct wifi_station *s, enum wifi_action action, uint64_t now) {
    if (command(s, action) == 1) s->deadline = fresh_time(s, now) + 6000000;
}

static void recover(struct wifi_station *s, uint64_t now) {
    /* Finish the outstanding command without reusing its request or key data. */
    if (s->configuring) {
        recover_command(s, s->pending_action, now);
        return;
    }
    bool drained = !s->io.drained || s->io.drained(s->io.context);
    bool remaining = s->scan_active || s->scan_session || s->joined || s->sta_live || s->bss_live;
    if ((!drained || remaining) && now >= s->deadline) {
        wifi_station_stop(s, "wireless cleanup timed out; reboot required");
        return;
    }
    if (!drained) return;
    if (s->scan_active) {
        recover_command(s, WIFI_SCAN_END, now);
        return;
    }
    if (s->scan_session) {
        recover_command(s, WIFI_SCAN_FINISH, now);
        return;
    }
    /* limProcessMlmDelStaRsp -> DEL_BSS -> limProcessMlmDelBssRsp -> IDLE.
     * Leaving POSTASSOC first can make firmware reject the subsequent deletes. */
    if (s->sta_live) {
        recover_command(s, WIFI_DELETE_STA, now);
        return;
    }
    if (s->bss_live) {
        recover_command(s, WIFI_DELETE_BSS, now);
        return;
    }
    if (s->joined) {
        recover_command(s, WIFI_LINK_IDLE, now);
        return;
    }
    wifi_auth_destroy(s->auth);
    s->auth = 0;
    s->found = false;
    s->sweeps = 0;
    s->aid = 0;
    s->rate_count = 0;
    s->probe_deadline = s->probe_next = 0;
    s->probe_attempts = 0;
    s->rsn = (struct wifi_rsn){0};
    memset(s->bssid, 0, 6);
    memset(s->sequence_seen, 0, sizeof(s->sequence_seen));
    enter(s, WIFI_BACKOFF, now);
    if (s->leaving) {
        s->deadline = now;
        return;
    } /* Left for standby: rejoin once it ends. */
    if (s->failures < 6) ++s->failures;
    unsigned seconds = 1u << (s->failures - 1);
    if (seconds > 16) seconds = 30;
    ++s->reconnects;
    s->deadline = now + (uint64_t)seconds * 1000000 +
                  (s->profile.mac[5] * 997u + s->reconnects * 7919u) % 250000;
}

static bool send_query(struct wifi_station *s, unsigned action, const unsigned char id[2]) {
    unsigned char frame[28];
    header(s, frame, 0x40d0);
    frame[24] = 8;
    frame[25] = action;
    memcpy(frame + 26, id, 2);
    return s->io.send(s->io.context, frame, sizeof(frame), false);
}

static void security_poll(struct wifi_station *s, uint64_t now) {
    if (!s->auth) return;
    wifi_auth_poll(s->auth, now);
    if (wifi_auth_failed(s->auth)) {
        wifi_station_reconnect(s, "wireless authentication failed", now);
        return;
    }
    if (wifi_auth_next_key(s->auth)) {
        if (!s->io.drained(s->io.context)) return;
        s->install_key = wifi_auth_next_key(s->auth);
        command(s, WIFI_INSTALL_KEY);
        return;
    }
    unsigned n;
    const unsigned char *p = wifi_auth_next_tx(s->auth, &n);
    if (p) {
        unsigned char frame[1600];
        header(s, frame, 0x0108 | (wifi_auth_pairwise(s->auth) ? 0x4000 : 0));
        memcpy(frame + 16, s->bssid, 6);
        memcpy(frame + 24, snap, 6);
        frame[30] = 0x88;
        frame[31] = 0x8e;
        memcpy(frame + 32, p, n);
        if (s->io.send(s->io.context, frame, n + 32, true)) wifi_auth_tx_done(s->auth);
    }
    if (s->query_active) {
        if (now >= s->query_deadline) {
            wifi_station_reconnect(s, "protected management query timed out", now);
            return;
        }
        if (now >= s->query_next && s->query_attempts < 5 && send_query(s, 0, s->query_id)) {
            ++s->query_attempts;
            s->query_next = now + 200000;
        }
    }
}

bool wifi_station_poll(struct wifi_station *s, uint64_t now) {
    if (!s || s->state == WIFI_FAILED || s->state == WIFI_OFF || s->state == WIFI_IDLE)
        return false;
    if (now > UINT64_MAX - 60000000) {
        wifi_station_stop(s, "wireless clock overflow");
        return false;
    }
    if (s->state == WIFI_RECOVER) {
        recover(s, now);
        return false;
    }
    if (s->state == WIFI_BACKOFF) {
        if (now >= s->deadline && !s->standby) {
            s->leaving = false;
            s->channel = s->profile.channel ? s->profile.channel : 1;
            enter(s, WIFI_SCAN_SETUP, now);
        }
        return false;
    }
    if (s->state == WIFI_HANDSHAKE || s->state == WIFI_ONLINE) security_poll(s, now);
    if (s->state == WIFI_RECOVER || s->state == WIFI_FAILED) return false;
    if (s->state == WIFI_ONLINE) {
        monitor_link(s, now);
        if (s->state == WIFI_ONLINE && now - s->online_since >= 30000000) s->failures = 0;
        return s->state == WIFI_ONLINE;
    }
    enum wifi_state before = s->state;
    switch (s->state) {
        case WIFI_SCAN_SETUP: configure(s, WIFI_SCAN_INIT, WIFI_SCAN_CHANNEL, now); break;
        case WIFI_SCAN_CHANNEL:
            if (configure(s, WIFI_SCAN_START, WIFI_SCANNING, now) == 1) s->deadline = now + 400000;
            break;
        case WIFI_SCANNING:
            if (s->found || now >= s->deadline) enter(s, WIFI_SCAN_STOP, now);
            break;
        case WIFI_SCAN_STOP:
            if (configure(s, WIFI_SCAN_END, WIFI_SCAN_CHANNEL, now) == 1) {
                if (s->found) enter(s, WIFI_SCAN_DONE, now);
                else if (s->profile.channel || ++s->channel > 11) {
                    s->channel = s->profile.channel ? s->profile.channel : 1;
                    if (s->survey) enter(s, WIFI_SCAN_DONE, now);
                    else if (++s->sweeps == 3)
                        wifi_station_reconnect(s, "configured access point not found", now);
                }
            }
            break;
        case WIFI_SCAN_DONE:
            if (s->survey) {
                configure(s, WIFI_SCAN_FINISH, WIFI_IDLE, now);
                break;
            }
            if (configure(s, WIFI_SCAN_FINISH, WIFI_JOINING, now) == 1 && s->profile.security) {
                s->auth = wifi_auth_create(&s->profile, s->bssid, &s->rsn, s->io.random,
                                           s->io.context, now);
                if (!s->auth) wifi_station_stop(s, "wireless secure authentication unavailable");
                else s->deadline = fresh_time(s, now) + 3000000;
            }
            break;
        case WIFI_JOINING: configure(s, WIFI_JOIN, WIFI_ADDING, now); break;
        case WIFI_ADDING: configure(s, WIFI_BSS_ADD, WIFI_AUTH, now); break;
        case WIFI_AUTH:
        case WIFI_ASSOC:
            if (s->sent && now < s->deadline) break;
            if (now >= s->deadline && s->attempts >= 3) {
                wifi_station_reconnect(s, "access point did not complete association", now);
                break;
            }
            if (management(s)) {
                s->sent = true;
                ++s->attempts;
                s->deadline = now + 1000000;
            } else if (now >= s->deadline)
                wifi_station_reconnect(s, "wireless transmit stalled", now);
            break;
        case WIFI_LINKING: configure(s, WIFI_LINK, WIFI_UPDATING, now); break;
        case WIFI_UPDATING: configure(s, WIFI_BSS_UPDATE, WIFI_STATION, now); break;
        case WIFI_STATION:
            if (configure(s, WIFI_STA, s->auth ? WIFI_HANDSHAKE : WIFI_ONLINE, now) == 1) {
                heard_ap(s, now);
                s->online_since = now;
                if (s->auth) {
                    s->deadline = now + 12000000;
                    if (!wifi_auth_associated(s->auth))
                        wifi_station_reconnect(s, "wireless authentication state invalid", now);
                }
            }
            break;
        case WIFI_HANDSHAKE:
            if (wifi_auth_ready(s->auth) && s->io.drained(s->io.context)) {
                s->state = WIFI_ONLINE;
                heard_ap(s, now);
                s->online_since = now;
                s->error = 0;
            }
            break;
        default: break;
    }
    /* The HAL or key exchange can have completed while the desktop was busy.
     * Consume that result before timing out a still-pending state. */
    if (!s->configuring && s->state == before && now >= s->deadline && s->state != WIFI_SCANNING &&
        s->state != WIFI_AUTH && s->state != WIFI_ASSOC)
        wifi_station_reconnect(s,
                               s->state == WIFI_HANDSHAKE ? "wireless key exchange timed out"
                                                          : "wireless association timed out",
                               now);
    return s->state == WIFI_ONLINE;
}

static bool rate_supported(unsigned rate) {
    for (unsigned i = 0; i < sizeof(supported); ++i)
        if (rate == supported[i]) return true;
    return false;
}

static void beacon(struct wifi_station *s, const unsigned char *p, unsigned n, uint64_t now) {
    if (n < 36 || !unicast(p + 10) || memcmp(p + 10, p + 16, 6) || (le16(p + 34) & 2) ||
        !(le16(p + 34) & 1))
        return;
    if (memcmp(p + 4, broadcast, 6) && memcmp(p + 4, s->profile.mac, 6)) return;
    bool channel = false, security = false, rates = false, h2e_only = false;
    unsigned rate_count = 0;
    unsigned char rate_values[12];
    const unsigned char *ssid = 0;
    unsigned ssid_length = 0;
    unsigned seen = 0;
    const unsigned char *rsn = 0, *extension = 0;
    unsigned rsn_length = 0, extension_length = 0;
    for (unsigned at = 36; at < n;) {
        if (n - at < 2) return;
        unsigned type = p[at++], len = p[at++];
        if (len > n - at) return;
        unsigned mask = type == 0     ? 1
                        : type == 1   ? 2
                        : type == 3   ? 4
                        : type == 48  ? 8
                        : type == 50  ? 16
                        : type == 244 ? 32
                                      : 0;
        if (mask && (seen & mask)) return;
        seen |= mask;
        if (type == 0) {
            if (!len || len > 32 || memchr(p + at, 0, len)) return;
            ssid = p + at;
            ssid_length = len;
        }
        if (type == 3) {
            if (len != 1 || p[at] != s->channel) return;
            channel = true;
        }
        if (type == 48 || (type == 221 && len >= 4 && !memcmp(p + at, "\0\x50\xf2\1", 4)))
            security = true;
        if (type == 48) {
            rsn = p + at - 2;
            rsn_length = len + 2;
        }
        if (type == 244) {
            extension = p + at - 2;
            extension_length = len + 2;
        }
        if (type == 1 || type == 50) {
            if (!len || (type == 1 && len > 8)) return;
            rates = true;
            for (unsigned i = 0; i < len; ++i) {
                unsigned value = p[at + i];
                /* BSS membership selector "SAE hash-to-element only" (123), not
                 * a rate. Every other required selector (HT, VHT, ...) is unsupported. */
                if (value == (128u | 123u)) {
                    h2e_only = true;
                    continue;
                }
                bool usable = rate_supported(value & 127);
                if ((value & 128) && !usable) return;
                if (usable) {
                    for (unsigned j = 0; j < rate_count; ++j)
                        if ((rate_values[j] & 127) == (value & 127)) return;
                    if (rate_count == 12) return;
                    rate_values[rate_count++] = value;
                }
            }
        }
        at += len;
    }
    if (!ssid || !channel || !rates || !(seen & 2) || !rate_count) return;
    struct wifi_rsn selected = {0};
    unsigned modes = 0;
    bool privacy = (le16(p + 34) & 16) != 0;
    if (privacy) {
        if (wifi_rsn_select(&selected, WIFI_WPA2_PSK, rsn, rsn_length, extension, extension_length))
            modes |= 1u << WIFI_WPA2_PSK;
        if (wifi_rsn_select(&selected, WIFI_WPA3_SAE, rsn, rsn_length, extension,
                            extension_length) &&
            (!h2e_only || selected.h2e))
            modes |= 1u << WIFI_WPA3_SAE;
    } else if (!security && !extension) modes = 1u << WIFI_OPEN;
    if (s->state == WIFI_SCANNING && s->io.discovered) {
        struct wifi_network network = {
            .ssid_length = ssid_length, .modes = modes, .channel = s->channel};
        memcpy(network.ssid, ssid, ssid_length);
        memcpy(network.bssid, p + 16, 6);
        s->io.discovered(s->io.context, &network);
    }
    if (s->survey || ssid_length != s->profile.ssid_length ||
        memcmp(ssid, s->profile.ssid, ssid_length) || privacy != !!s->profile.security)
        return;
    if (s->profile.security) {
        if (!wifi_rsn_select(&selected, s->profile.security, rsn, rsn_length, extension,
                             extension_length))
            return;
    } else if (security || extension) return;
    if (h2e_only && s->profile.security == WIFI_WPA3_SAE && !selected.h2e) return;
    if (s->state == WIFI_ONLINE && !memcmp(p + 16, s->bssid, 6)) {
        heard_ap(s, now);
        return;
    }
    if (s->state != WIFI_SCANNING || s->found) return;
    memcpy(s->bssid, p + 16, 6);
    memcpy(s->rates, rate_values, rate_count);
    s->rate_count = rate_count;
    s->rsn = selected;
    s->beacon_interval = le16(p + 32);
    s->capabilities = le16(p + 34);
    s->found = true;
    heard_ap(s, now);
}

static bool verified_management(struct wifi_station *s, const unsigned char *p, unsigned n,
                                const struct wifi_rx_status *rx) {
    if (!s->auth || !wifi_auth_ready(s->auth)) return false;
    if (!(p[4] & 1))
        return (le16(p) & 0x4000) && rx->decrypted && rx->management && rx->key_index == 0;
    return wifi_auth_verify_group_management(s->auth, p, n) ||
           (rx->decrypted && rx->management &&
            wifi_auth_has_key(s->auth, WIFI_KEY_INTEGRITY, rx->key_index));
}

void wifi_station_receive_checked(struct wifi_station *s, const unsigned char *p, unsigned n,
                                  const struct wifi_rx_status *rx, uint64_t now) {
    if (!s || !p || !rx || n < 24 || n > 2304 || s->state == WIFI_OFF || s->state == WIFI_FAILED ||
        s->state == WIFI_RECOVER || s->state == WIFI_BACKOFF)
        return;
    unsigned fc = le16(p), type = fc & 0xfc;
    if ((fc & 3) || (fc & 0x8400) || (le16(p + 22) & 15))
        return; /* version, fragments, ordered frames */
    if ((type == 0x80 || type == 0x50) && !(fc & 0x4300)) {
        beacon(s, p, n, now);
        return;
    }
    if (memcmp(p + 10, s->bssid, 6)) return;
    if (type == 0xc0 || type == 0xa0) {
        if (n < 26 || (fc & 0x0300) || memcmp(p + 16, s->bssid, 6) ||
            (memcmp(p + 4, s->profile.mac, 6) && memcmp(p + 4, broadcast, 6)))
            return;
        if (s->rsn.pmf && s->state == WIFI_ONLINE && !verified_management(s, p, n, rx)) {
            unsigned reason = le16(p + 24);
            if (!(p[4] & 1) && !(fc & 0x4000) && (reason == 6 || reason == 7) && !s->query_active) {
                if (s->io.random(s->io.context, s->query_id, 2)) {
                    wifi_station_stop(s, "wireless entropy failed");
                    return;
                }
                s->query_active = true;
                s->query_attempts = 0;
                s->query_next = now;
                s->query_deadline = now + 1000000;
            }
            return;
        }
        if ((fc & 0x4000) && !verified_management(s, p, n, rx)) return;
        /* Retain the AP's reason through cleanup/backoff. An OTA ACK alone
         * cannot distinguish an accepted data packet from decryption or
         * handshake failure at the AP. Apply the security checks above first. */
        static const char prefix[] = "access point disconnected: reason ";
        unsigned at = sizeof(prefix) - 1, reason = le16(p + 24);
        char digits[5];
        unsigned count = 0;
        memcpy(s->disconnect_detail, prefix, at);
        do {
            digits[count++] = (char)('0' + reason % 10);
            reason /= 10;
        } while (reason);
        while (count) s->disconnect_detail[at++] = digits[--count];
        s->disconnect_detail[at] = 0;
        wifi_station_reconnect(s, s->disconnect_detail, now);
        return;
    }
    if (type == 0xd0 && s->state == WIFI_ONLINE && s->rsn.pmf) {
        if (n != 28 || (fc & 0x0300) || memcmp(p + 4, s->profile.mac, 6) ||
            memcmp(p + 16, s->bssid, 6) || p[24] != 8 || !verified_management(s, p, n, rx))
            return;
        if (p[25] == 0) send_query(s, 1, p + 26);
        else if (p[25] == 1 && s->query_active && !memcmp(p + 26, s->query_id, 2))
            s->query_active = false;
        if (p[25] <= 1) heard_ap(s, now);
        return;
    }
    if (type == 0xb0 && s->state == WIFI_AUTH && s->sent) {
        if (n < 30 || (fc & 0x4300) || memcmp(p + 4, s->profile.mac, 6) ||
            memcmp(p + 16, s->bssid, 6))
            return;
        if (s->profile.security == WIFI_WPA3_SAE) {
            int result = wifi_auth_sae_rx(s->auth, p + 24, n - 24);
            now = fresh_time(s, now);
            if (result < 0) wifi_station_reconnect(s, "SAE authentication rejected", now);
            else if (result == 1) {
                /* Send our Confirm now: an AP Confirm may already follow in this
                 * receive pass and is only accepted once ours has been sent. */
                s->sent = false;
                s->deadline = now + 1000000;
                s->attempts = 0;
                if (management(s)) {
                    s->sent = true;
                    s->attempts = 1;
                }
            } else if (result == 2) enter(s, WIFI_ASSOC, now);
            return;
        }
        if (le16(p + 24) || le16(p + 26) != 2) return;
        for (unsigned at = 30; at < n;) {
            if (n - at < 2 || p[at + 1] > n - at - 2) return;
            at += p[at + 1] + 2;
        }
        if (le16(p + 28)) wifi_station_reconnect(s, "access point rejected authentication", now);
        else enter(s, WIFI_ASSOC, now);
        return;
    }
    if (type == 0x10 && s->state == WIFI_ASSOC && s->sent) {
        if (n < 30 || (fc & 0x4300) || memcmp(p + 4, s->profile.mac, 6) ||
            memcmp(p + 16, s->bssid, 6))
            return;
        if (le16(p + 26) || !(le16(p + 24) & 1) || (le16(p + 24) & 2) ||
            !!(le16(p + 24) & 16) != !!s->profile.security) {
            wifi_station_reconnect(s, "access point rejected association", now);
            return;
        }
        unsigned aid = le16(p + 28) & 0x3fff;
        if (!aid || aid > 2007 || (le16(p + 28) & 0xc000) != 0xc000) return;
        bool rates = false, rsn = false;
        for (unsigned at = 30; at < n;) {
            if (n - at < 2 || p[at + 1] > n - at - 2) return;
            unsigned type = p[at], length = p[at + 1];
            at += 2;
            if (type == 48) {
                if (rsn || !s->auth ||
                    !wifi_rsn_response(&s->rsn, s->profile.security, p + at - 2, length + 2))
                    return;
                rsn = true;
            }
            if (!s->auth && type == 221 && length >= 4 && !memcmp(p + at, "\0\x50\xf2\1", 4))
                return;
            if (type == 1) {
                if (rates || !length || length > 8) return;
                rates = true;
            }
            if (type == 1 || type == 50)
                for (unsigned i = 0; i < length; ++i)
                    if ((p[at + i] & 128) && p[at + i] != (128u | 123u) &&
                        !rate_supported(p[at + i] & 127))
                        return;
            at += length;
        }
        if (!rates) return;
        s->aid = aid;
        enter(s, WIFI_LINKING, now);
        return;
    }
    if ((s->state != WIFI_ONLINE && s->state != WIFI_HANDSHAKE) || (type != 0x08 && type != 0x88) ||
        (fc & 0x0300) != 0x0200)
        return;
    if ((memcmp(p + 4, s->profile.mac, 6) && !(p[4] & 1)) || !unicast(p + 16)) return;
    ++s->rx_data_seen;
    unsigned header = 24, tid = 16;
    if (type == 0x88) {
        if (n < 26 || (p[24] & 0x80)) return;
        tid = p[24] & 15;
        if (tid > 7) return;
        header = 26;
    }
    unsigned payload = rx->data_offset ? rx->data_offset : header;
    if (payload < header || payload > n || n - payload < 8 || memcmp(p + payload, snap, 6) ||
        p[payload + 6] < 6)
        return;
    bool protected_frame = fc & 0x4000, eapol = p[payload + 6] == 0x88 && p[payload + 7] == 0x8e;
    if (protected_frame &&
        (!s->auth || !rx->decrypted || rx->management ||
         !wifi_auth_has_key(s->auth, (p[4] & 1) ? WIFI_KEY_GROUP : WIFI_KEY_PAIRWISE,
                            rx->key_index)))
        return;
    if (!eapol && (s->state != WIFI_ONLINE || (s->auth && !protected_frame))) return;
    if (eapol && (!s->auth || memcmp(p + 4, s->profile.mac, 6) || memcmp(p + 16, s->bssid, 6)))
        return;
    unsigned length = n - payload - 8 + 14;
    if (length > 1514) return;
    unsigned seq = le16(p + 22);
    if ((fc & 0x800) && s->sequence_seen[tid] && s->last_sequence[tid] == seq) return;
    s->last_sequence[tid] = seq;
    s->sequence_seen[tid] = true;
    ++s->rx_data_admitted;
    /* Only frames admitted by the address, integrity/key and replay checks
     * above count as activity. Untrusted traffic cannot postpone reconnect. */
    if (!eapol || protected_frame) heard_ap(s, now);
    if (eapol) {
        wifi_auth_eapol(s->auth, p + payload + 8, n - payload - 8, protected_frame);
        return;
    }
    unsigned char ethernet[1514];
    memcpy(ethernet, p + 4, 6);
    memcpy(ethernet + 6, p + 16, 6);
    memcpy(ethernet + 12, p + payload + 6, n - payload - 6);
    s->io.receive(s->io.context, ethernet, length);
}

bool wifi_station_send(struct wifi_station *s, const void *data, unsigned n) {
    const unsigned char *p = data;
    if (!s || s->state != WIFI_ONLINE || wifi_auth_busy(s->auth) || !p || n < 14 || n > 1514 ||
        p[12] < 6 || memcmp(p + 6, s->profile.mac, 6) || (p[12] == 0x88 && p[13] == 0x8e))
        return false;
    unsigned char frame[1536];
    header(s, frame, 0x0108 | (s->auth ? 0x4000 : 0));
    memcpy(frame + 16, p, 6);
    memcpy(frame + 24, snap, 6);
    memcpy(frame + 30, p + 12, n - 12);
    return s->io.send(s->io.context, frame, n + 18, true);
}

const char *wifi_station_status(const struct wifi_station *s) {
    if (!s || s->state == WIFI_OFF) return "Wi-Fi unavailable";
    if (s->state == WIFI_IDLE) return "Choose a Wi-Fi network";
    if (s->state == WIFI_FAILED) return s->error ? s->error : "Wi-Fi hardware failed";
    if (s->state == WIFI_RECOVER || s->state == WIFI_BACKOFF)
        return s->error ? s->error : "Wi-Fi reconnecting";
    if (s->state == WIFI_ONLINE)
        return s->profile.security == WIFI_WPA3_SAE   ? "Wi-Fi WPA3 connected"
               : s->profile.security == WIFI_WPA2_PSK ? "Wi-Fi WPA2 connected"
                                                      : "Wi-Fi open connected";
    if (s->state <= WIFI_SCAN_DONE) return "Wi-Fi scanning";
    if (s->state == WIFI_HANDSHAKE) return "Wi-Fi installing secure keys";
    return "Wi-Fi authenticating";
}
