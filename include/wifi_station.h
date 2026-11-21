#ifndef SOS2_WIFI_STATION_H
#define SOS2_WIFI_STATION_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "wifi_auth.h"

enum wifi_action {
    WIFI_SCAN_INIT,
    WIFI_SCAN_START,
    WIFI_SCAN_END,
    WIFI_SCAN_FINISH,
    WIFI_JOIN,
    WIFI_BSS_ADD,
    WIFI_LINK,
    WIFI_BSS_UPDATE,
    WIFI_STA,
    WIFI_INSTALL_KEY,
    WIFI_LINK_IDLE,
    WIFI_DELETE_STA,
    WIFI_DELETE_BSS
};

enum wifi_state {
    WIFI_OFF,
    WIFI_SCAN_SETUP,
    WIFI_SCAN_CHANNEL,
    WIFI_SCANNING,
    WIFI_SCAN_STOP,
    WIFI_SCAN_DONE,
    WIFI_JOINING,
    WIFI_ADDING,
    WIFI_AUTH,
    WIFI_ASSOC,
    WIFI_LINKING,
    WIFI_UPDATING,
    WIFI_STATION,
    WIFI_HANDSHAKE,
    WIFI_ONLINE,
    WIFI_RECOVER,
    WIFI_BACKOFF,
    WIFI_FAILED,
    WIFI_IDLE
};
struct wifi_station;

struct wifi_ops {
    /* Return 0 while pending, 1 when acknowledged, -1 on failure. The backend
     * must bound its send/reply waits; station timers must not override them. */
    int (*configure)(void *, enum wifi_action, const struct wifi_station *);
    bool (*send)(void *, const void *, unsigned, bool data);
    void (*receive)(void *, const void *, unsigned);
    void *context;
    int (*random)(void *, unsigned char *, size_t);
    bool (*drained)(void *);
    uint64_t (*clock)(void *);
    void (*discovered)(void *, const struct wifi_network *);
};

/* Protected data is admitted only after firmware integrity/replay validation.
 * Unmodified BIP frames can instead be verified against the private IGTK. */
/* data_offset locates LLC within a received data MPDU. Zero denotes a compact
 * raw 802.11 frame without hardware padding (e.g. a software radio). */
struct wifi_rx_status {
    bool decrypted, management;
    unsigned key_index, data_offset;
};

struct wifi_station {
    struct wifi_profile profile;
    struct wifi_ops io;
    enum wifi_state state;
    const char *error;
    char disconnect_detail[48];
    unsigned char bssid[6], rates[12];
    unsigned channel, rate_count, beacon_interval, capabilities, aid, sequence, attempts, sweeps;
    uint64_t deadline, last_rx, probe_deadline, probe_next;
    bool found, sent;
    struct wifi_rsn rsn;
    struct wifi_auth *auth;
    const struct wifi_key *install_key;
    enum wifi_action pending_action;
    bool configuring, scan_active, scan_session, joined, bss_live, sta_live, query_active;
    unsigned reconnects, failures, query_attempts, probe_attempts;
    uint64_t query_deadline, query_next, online_since;
    unsigned char query_id[2];
    uint16_t last_sequence[17];
    bool sequence_seen[17];
    bool survey;
    /* Standby holds reconnection scans; leaving marks a network left for
     * standby, rejoined once it ends without counting a failure. */
    bool standby, leaving;
};

bool wifi_station_start(struct wifi_station *, const struct wifi_profile *, const struct wifi_ops *,
                        uint64_t);
bool wifi_station_survey(struct wifi_station *, const unsigned char mac[6], const struct wifi_ops *,
                         uint64_t);
bool wifi_station_poll(struct wifi_station *, uint64_t);
void wifi_station_receive_checked(struct wifi_station *, const unsigned char *, unsigned,
                                  const struct wifi_rx_status *, uint64_t);
bool wifi_station_send(struct wifi_station *, const void *, unsigned);
void wifi_station_stop(struct wifi_station *, const char *);
void wifi_station_reconnect(struct wifi_station *, const char *, uint64_t);
void wifi_station_beacon_missed(struct wifi_station *, uint64_t);
/* A FullMAC radio's news, for a firmware that authenticates, associates and
 * keeps the link itself (boards/nexus5/wifi.c), in place of the frames a
 * SoftMAC radio would hear: the station's authentication answered (open
 * system), its association done (the network joined), the link lost with
 * the access point's reason, and the network heard while the firmware holds
 * the link. Each acts only in the state such a frame would. */
void wifi_station_authenticated(struct wifi_station *, uint64_t);
void wifi_station_associated(struct wifi_station *, uint64_t);
void wifi_station_disconnected(struct wifi_station *, unsigned reason, uint64_t);
void wifi_station_heard(struct wifi_station *, uint64_t);
/* Set by the radio driver before each poll. Standby leaves the network, as a
 * reconnection does (the station and BSS deleted) but without an error, and
 * holds the rejoin until standby ends; a survey runs to its end. */
void wifi_station_standby(struct wifi_station *, bool standby, uint64_t);
const char *wifi_station_status(const struct wifi_station *);
#endif
