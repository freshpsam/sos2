#ifndef SOS2_WIFI_AUTH_H
#define SOS2_WIFI_AUTH_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "wifi_profile.h"

enum wifi_key_type { WIFI_KEY_PAIRWISE, WIFI_KEY_GROUP, WIFI_KEY_INTEGRITY };

struct wifi_key {
    enum wifi_key_type type;
    unsigned index;
    unsigned char key[16], sequence[8];
};

struct wifi_rsn {
    unsigned char ie[257], extension[32];
    unsigned length, extension_length;
    bool pmf, h2e;
};
struct wifi_auth;
bool wifi_auth_credential(struct wifi_profile *, const unsigned char *, unsigned);
bool wifi_rsn_select(struct wifi_rsn *, unsigned security, const unsigned char *, unsigned,
                     const unsigned char *, unsigned);
bool wifi_rsn_response(const struct wifi_rsn *, unsigned security, const unsigned char *, unsigned);
struct wifi_auth *wifi_auth_create(const struct wifi_profile *, const unsigned char bssid[6],
                                   const struct wifi_rsn *,
                                   int (*random)(void *, unsigned char *, size_t), void *,
                                   uint64_t);
void wifi_auth_destroy(struct wifi_auth *);
bool wifi_auth_failed(const struct wifi_auth *);
bool wifi_auth_ready(const struct wifi_auth *);
bool wifi_auth_pairwise(const struct wifi_auth *);
bool wifi_auth_has_key(const struct wifi_auth *, enum wifi_key_type, unsigned);
bool wifi_auth_busy(const struct wifi_auth *);
void wifi_auth_poll(struct wifi_auth *, uint64_t);
unsigned wifi_auth_association_ie(struct wifi_auth *, unsigned char *, unsigned);
unsigned wifi_auth_sae_tx(struct wifi_auth *, unsigned char *, unsigned);
/* -1 failed, 0 ignored, 1 send next SAE message, 2 SAE accepted. */
int wifi_auth_sae_rx(struct wifi_auth *, const unsigned char *, unsigned);
bool wifi_auth_associated(struct wifi_auth *);
void wifi_auth_eapol(struct wifi_auth *, const unsigned char *, unsigned, bool protected_frame);
/* Only the earliest queued operation is exposed. Transmit completion here
 * means submitted to DMA; the caller must drain DMA before installing a key. */
const struct wifi_key *wifi_auth_next_key(struct wifi_auth *);
void wifi_auth_key_done(struct wifi_auth *);
const unsigned char *wifi_auth_next_tx(struct wifi_auth *, unsigned *);
void wifi_auth_tx_done(struct wifi_auth *);
bool wifi_auth_verify_group_management(struct wifi_auth *, const unsigned char *, unsigned);
#endif
