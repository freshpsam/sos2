#ifndef SOS2_WIFI_PROFILE_H
#define SOS2_WIFI_PROFILE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum wifi_security { WIFI_OPEN, WIFI_WPA2_PSK, WIFI_WPA3_SAE };

/* Leave room for later-scanned channels in crowded deployments. */
enum { WIFI_PROFILE_BYTES = 160, WIFI_NETWORK_MAX = 128 };

struct wifi_profile {
    unsigned char mac[6], ssid[32], pmk[32], password[64];
    unsigned ssid_length, security, channel, password_length;
};

/* A discovery result contains no credentials. Modes is a bitset of wifi_security. */
struct wifi_network {
    unsigned char ssid[32], bssid[6];
    unsigned ssid_length, modes, channel;
};

bool wifi_mac_valid(const unsigned char *);
bool wifi_profile_read(struct wifi_profile *, const void *, size_t);
bool wifi_profile_write(const struct wifi_profile *, unsigned char out[WIFI_PROFILE_BYTES]);
void wifi_secret_clear(void *, size_t);
#endif
