#ifndef SOS2_WIFI_SETUP_H
#define SOS2_WIFI_SETUP_H
#include "wifi_profile.h"

enum wifi_radio_state {
    WIFI_RADIO_UNAVAILABLE,
    WIFI_RADIO_STARTING,
    WIFI_RADIO_IDLE,
    WIFI_RADIO_SCANNING,
    WIFI_RADIO_CONNECTING,
    WIFI_RADIO_CONNECTED,
    WIFI_RADIO_FAILED
};

struct wifi_radio_view {
    enum wifi_radio_state state;
    const char *message;
    struct wifi_network current;
    unsigned count, request_id, scan_generation;
    bool scan_truncated; /* A distinct network did not fit in the scan list. */
    bool attempt_failed;
    bool startup_pending; /* Includes entropy preparation and failure cleanup. */
    /* Last, with only the first count entries set. A scan only appends, so
     * the list changes only with count or scan_generation. */
    struct wifi_network networks[WIFI_NETWORK_MAX];
};

struct wifi_setup_view {
    struct wifi_network saved;
    const char *message;
    /* storage means durable; has_saved also covers a RAM-only session profile. */
    bool available, busy, storage, session, has_saved, saving;
    struct wifi_radio_view radio; /* Last: the network list ends the view. */
};

/* All backend methods run on the existing serialized service worker. Only
 * pending() is called on the main loop, and must be read-only and bounded. */
struct wifi_setup_ops {
    bool (*start)(const unsigned char mac[6]);
    bool (*pending)(void);
    void (*service)(void *);
    void (*snapshot)(struct wifi_radio_view *);
    void (*scan)(void);
    unsigned (*connect)(const struct wifi_profile *); /* NULL disconnects. */
    bool (*credential)(struct wifi_profile *, const unsigned char *, unsigned);
};

void wifi_setup_init(const struct wifi_setup_ops *, const struct wifi_profile *bootstrap);
const struct wifi_setup_view *wifi_setup_view(void);
/* The saved network's password for its settings page: its length, copied
 * into out (64 bytes, unterminated) if out is given; 0 for an open network
 * or a WPA2 profile imported as a PMK alone. */
unsigned wifi_setup_saved_password(char *out);
bool wifi_setup_scan(void);
bool wifi_setup_connect(const struct wifi_network *, unsigned security,
                        const unsigned char *password, unsigned length, bool saved);
bool wifi_setup_forget(void);
/* Standby entry and exit: a connection attempt's time limit pauses, since
 * standby keeps the station off its network. */
void wifi_setup_standby(bool on);
bool wifi_setup_pending(void);
bool wifi_setup_startup_pending(void);
void wifi_setup_service(void *);
/* Worker progress before a potentially blocking hardware operation. The
 * message must remain valid until the backend's next snapshot/progress. */
void wifi_setup_progress(const char *message);
#endif
