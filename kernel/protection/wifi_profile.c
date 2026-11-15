#include "wifi_profile.h"
#include "platform.h"

void wifi_secret_clear(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

bool wifi_mac_valid(const unsigned char *p) {
    return p && !(p[0] & 1) && memcmp(p, "\0\0\0\0\0\0", 6);
}

bool wifi_profile_read(struct wifi_profile *p, const void *data, size_t size) {
    if (!p) return false;
    *p = (struct wifi_profile){0};
    const unsigned char *b = data;
    if (!b || (size != 80 && size != 160) || memcmp(b, "SOWF", 4) || !b[5] || b[5] > 32 ||
        b[6] > WIFI_WPA3_SAE || b[7] > 11 || !wifi_mac_valid(b + 8) || b[15])
        return false;
    if ((b[4] == 1 && (size != 80 || b[6] || b[14])) || (b[4] == 2 && size != 160) ||
        (b[4] != 1 && b[4] != 2))
        return false;
    for (unsigned i = 16 + b[5]; i < 48; ++i)
        if (b[i]) return false;
    for (unsigned i = 0; i < b[5]; ++i)
        if (!b[16 + i]) return false;
    /* SAE needs its password; WPA2 keeps one beside its PMK unless it was
     * imported as a PMK alone. */
    unsigned password = b[14];
    if (b[6] == WIFI_OPEN && password) return false;
    if (b[6] == WIFI_WPA3_SAE && (!password || password > 63)) return false;
    if (b[6] == WIFI_WPA2_PSK && password && (password < 8 || password > 63)) return false;
    if (b[6] != WIFI_WPA2_PSK)
        for (unsigned i = 48; i < 80; ++i)
            if (b[i]) return false;
    if (size == 160) {
        for (unsigned i = 80 + password; i < 160; ++i)
            if (b[i]) return false;
        for (unsigned i = 0; i < password; ++i)
            if (!b[80 + i]) return false;
    }
    memcpy(p->mac, b + 8, 6);
    memcpy(p->ssid, b + 16, b[5]);
    memcpy(p->pmk, b + 48, 32);
    if (password) memcpy(p->password, b + 80, password);
    p->ssid_length = b[5];
    p->security = b[6];
    p->channel = b[7];
    p->password_length = password;
    return true;
}

bool wifi_profile_write(const struct wifi_profile *p, unsigned char out[WIFI_PROFILE_BYTES]) {
    if (!p || !out || !wifi_mac_valid(p->mac) || !p->ssid_length || p->ssid_length > 32 ||
        p->security > WIFI_WPA3_SAE || p->channel > 11 || p->password_length > 63)
        return false;
    memset(out, 0, WIFI_PROFILE_BYTES);
    memcpy(out, "SOWF", 4);
    out[4] = 2;
    out[5] = p->ssid_length;
    out[6] = p->security;
    out[7] = p->channel;
    memcpy(out + 8, p->mac, 6);
    memcpy(out + 16, p->ssid, p->ssid_length);
    if (p->security == WIFI_WPA2_PSK) memcpy(out + 48, p->pmk, 32);
    if (p->security != WIFI_OPEN) {
        out[14] = p->password_length;
        memcpy(out + 80, p->password, p->password_length);
    }
    struct wifi_profile check;
    bool valid = wifi_profile_read(&check, out, WIFI_PROFILE_BYTES);
    wifi_secret_clear(&check, sizeof(check));
    if (!valid) wifi_secret_clear(out, WIFI_PROFILE_BYTES);
    return valid;
}
