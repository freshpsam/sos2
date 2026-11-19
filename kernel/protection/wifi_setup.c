#include "wifi_setup.h"
#include "fs.h"
#include "entropy.h"
#include "page_host.h"
#include "desktop_wifi.h"
#include "platform.h"
#include "wire.h"
#include "sha256.h"

enum { SETTINGS_SIZE = 188 };

enum { COMMAND_NONE, COMMAND_SCAN, COMMAND_CONNECT, COMMAND_FORGET };
static struct wifi_setup_ops backend;
static struct wifi_setup_view view;
static struct wifi_profile saved_profile, bootstrap_profile, candidate;
static unsigned char identity[6], password[64];
static unsigned command, password_length, candidate_id;
static bool initializing, working, attempting, have_bootstrap, use_saved;
static uint64_t attempt_deadline;
/* When standby began, 0 while awake: the station waits it out off its
 * network, so a connection attempt's minute stops meanwhile. */
static uint64_t standby_since;

static bool decode(const unsigned char *data, unsigned n) {
    if (n != SETTINGS_SIZE || memcmp(data, "SOWS", 4) || le32(data + 4) != 1 ||
        le32(data + 8) > 1 || le32(data + 12) || data[22] || data[23] ||
        !wifi_mac_valid(data + 16) || le32(data + 184) != crc32_ieee(data, 184))
        return false;
    bool present = le32(data + 8) != 0;
    if (present) {
        if (!wifi_profile_read(&saved_profile, data + 24, 160) ||
            memcmp(saved_profile.mac, data + 16, 6))
            return false;
    } else
        for (unsigned i = 24; i < 184; ++i)
            if (data[i]) return false;
    memcpy(identity, data + 16, 6);
    view.has_saved = present;
    return true;
}

static bool store_profile(const struct wifi_profile *profile) {
    unsigned char data[SETTINGS_SIZE] = {0};
    memcpy(data, "SOWS", 4);
    put32(data + 4, 1);
    put32(data + 8, profile != 0);
    memcpy(data + 16, identity, 6);
    bool valid = !profile || wifi_profile_write(profile, data + 24);
    put32(data + 184, crc32_ieee(data, 184));
    int result = valid ? fs_settings_write(FS_SETTING_WIFI, data, sizeof(data)) : USER_EINVAL;
    wifi_secret_clear(data, sizeof(data));
    return result == 0;
}

static struct wifi_network profile_name(const struct wifi_profile *p) {
    struct wifi_network name = {
        .ssid_length = p->ssid_length, .modes = 1u << p->security, .channel = p->channel};
    memcpy(name.ssid, p->ssid, p->ssid_length);
    return name;
}

void wifi_setup_init(const struct wifi_setup_ops *ops, const struct wifi_profile *bootstrap) {
    backend = ops ? *ops : (struct wifi_setup_ops){0};
    view = (struct wifi_setup_view){0};
    wifi_secret_clear(&saved_profile, sizeof(saved_profile));
    wifi_secret_clear(&candidate, sizeof(candidate));
    wifi_secret_clear(&bootstrap_profile, sizeof(bootstrap_profile));
    wifi_secret_clear(password, sizeof(password));
    memset(identity, 0, sizeof(identity));
    have_bootstrap = bootstrap != 0;
    if (bootstrap) bootstrap_profile = *bootstrap;
    command = COMMAND_NONE;
    password_length = candidate_id = 0;
    working = attempting = use_saved = false;
    initializing = view.available = backend.start && backend.pending && backend.service &&
                                    backend.snapshot && backend.scan && backend.connect &&
                                    backend.credential;
    view.busy = initializing;
    view.radio.state = initializing ? WIFI_RADIO_STARTING : WIFI_RADIO_UNAVAILABLE;
    view.message = initializing ? "Loading Wi-Fi settings" : "Wi-Fi hardware unavailable";
}

const struct wifi_setup_view *wifi_setup_view(void) { return &view; }

unsigned wifi_setup_saved_password(char *out) {
    unsigned n = view.has_saved ? saved_profile.password_length : 0;
    if (out && n) memcpy(out, saved_profile.password, n);
    return n;
}

void wifi_setup_progress(const char *message) { view.radio.message = message; }

static bool queue(unsigned next) {
    if (!view.available || initializing || working || command != COMMAND_NONE) return false;
    command = next;
    view.busy = true;
    page_host_redraw();
    return true;
}

bool wifi_setup_scan(void) { return queue(COMMAND_SCAN); }

bool wifi_setup_forget(void) { return view.has_saved && queue(COMMAND_FORGET); }

bool wifi_setup_connect(const struct wifi_network *network, unsigned security,
                        const unsigned char *secret, unsigned n, bool saved) {
    if (!network || !network->ssid_length || network->ssid_length > 32 ||
        security > WIFI_WPA3_SAE || !(network->modes & (1u << security)) ||
        (!view.storage && !view.session))
        return false;
    if (n > 63 || (n && !secret) || (saved && n)) return false;
    if (saved) {
        if (!view.has_saved || saved_profile.security != security ||
            saved_profile.ssid_length != network->ssid_length ||
            memcmp(saved_profile.ssid, network->ssid, network->ssid_length))
            return false;
    } else if ((security == WIFI_OPEN && n) ||
               (security != WIFI_OPEN && (!secret || n < (security == WIFI_WPA2_PSK ? 8u : 1u) ||
                                          n > 63 || memchr(secret, 0, n))))
        return false;
    if (!queue(COMMAND_CONNECT)) return false;
    wifi_secret_clear(&candidate, sizeof(candidate));
    wifi_secret_clear(password, sizeof(password));
    candidate = (struct wifi_profile){.ssid_length = network->ssid_length, .security = security};
    memcpy(candidate.ssid, network->ssid, network->ssid_length);
    memcpy(candidate.mac, identity, 6);
    use_saved = saved;
    password_length = n;
    if (n) memcpy(password, secret, n);
    return true;
}

void wifi_setup_standby(bool on) {
    uint64_t now = time_us();
    if (on) {
        if (!standby_since) standby_since = now ? now : 1;
        return;
    }
    if (standby_since && attempting) attempt_deadline += now - standby_since;
    standby_since = 0;
}

bool wifi_setup_pending(void) {
    return initializing || command != COMMAND_NONE || (view.available && backend.pending());
}

bool wifi_setup_startup_pending(void) { return initializing || view.radio.startup_pending; }

static void load(void) {
    unsigned char data[SETTINGS_SIZE];
    int n = fs_settings_read(FS_SETTING_WIFI, data, sizeof(data));
    bool loaded = n >= 0 && decode(data, (unsigned)n);
    view.storage = fs_persistent_ready();
    view.session = fs_session_only();
    view.message = 0;
    if (!loaded) {
        wifi_secret_clear(&saved_profile, sizeof(saved_profile));
        view.has_saved = false;
        /* Take only the identity: imported credentials are used through
         * `candidate` and remembered only after authentication. */
        if (have_bootstrap) memcpy(identity, bootstrap_profile.mac, 6);
        else {
            if (entropy_read(identity, sizeof(identity)) != sizeof(identity)) {
                view.message = "Secure randomness unavailable";
                view.radio.state = WIFI_RADIO_FAILED;
                view.available = false;
                goto done;
            }
            identity[0] = (identity[0] & 0xfc) | 2;
        }
        /* A new identity is saved at once. An imported profile brings its
         * own, and the record waits until the import connects: one that does
         * not (its network out of range, say) is tried again next boot. */
        if (n != USER_ENOENT) view.message = "Could not read Wi-Fi settings";
        else if (!have_bootstrap && !store_profile(0))
            view.message = "Settings storage unavailable";
    }
    if (!backend.start(identity)) {
        backend.snapshot(&view.radio);
        view.message = view.radio.message;
        goto done;
    }
    if (view.has_saved) backend.connect(&saved_profile);
    else if (!loaded && n == USER_ENOENT && have_bootstrap) {
        /* Remember imported credentials only after authentication; in RAM
         * mode the profile and device identity disappear on reboot. */
        candidate = bootstrap_profile;
        memcpy(candidate.mac, identity, 6);
        candidate_id = backend.connect(&candidate);
        attempting = candidate_id != 0;
        attempt_deadline = time_us() + 60000000;
    }
done:
    wifi_secret_clear(data, sizeof(data));
    wifi_secret_clear(&bootstrap_profile, sizeof(bootstrap_profile));
    have_bootstrap = false;
    initializing = false;
}

void wifi_setup_service(void *unused) {
    (void)unused;
    if (!view.available) return;
    /* The view up to its network list decides the redraw (include/wifi_setup.h). */
    struct wifi_setup_view before;
    memcpy(&before, &view, offsetof(struct wifi_setup_view, radio.networks));
    working = true;
    if (initializing) load();
    unsigned action = command;
    command = COMMAND_NONE;
    if (action == COMMAND_SCAN) {
        attempting = false;
        wifi_secret_clear(&candidate, sizeof(candidate));
        backend.connect(view.has_saved ? &saved_profile : 0);
        backend.scan();
        view.message = 0;
    } else if (action == COMMAND_FORGET) {
        view.saving = true;
        page_host_redraw();
        if (store_profile(0)) {
            attempting = false;
            view.has_saved = false;
            wifi_secret_clear(&saved_profile, sizeof(saved_profile));
            wifi_secret_clear(&candidate, sizeof(candidate));
            backend.connect(0);
            view.message = view.session ? "Session network forgotten" : "Saved network forgotten";
        } else view.message = "Could not forget network";
        view.saving = false;
    } else if (action == COMMAND_CONNECT) {
        attempting = false;
        if (use_saved) candidate = saved_profile;
        bool valid = use_saved || backend.credential(&candidate, password, password_length);
        wifi_secret_clear(password, sizeof(password));
        password_length = 0;
        if (valid && (candidate_id = backend.connect(&candidate))) {
            attempting = true;
            attempt_deadline = time_us() + 60000000;
            view.message = "Connecting...";
        } else {
            wifi_secret_clear(&candidate, sizeof(candidate));
            view.message = "Could not prepare connection";
        }
    }
    if (view.available) backend.service(0);
    if (view.available) backend.snapshot(&view.radio);
    if (view.radio.count > WIFI_NETWORK_MAX) view.radio.count = WIFI_NETWORK_MAX;
    if (attempting && view.radio.request_id == candidate_id &&
        view.radio.state == WIFI_RADIO_CONNECTED &&
        view.radio.current.ssid_length == candidate.ssid_length &&
        !memcmp(view.radio.current.ssid, candidate.ssid, candidate.ssid_length) &&
        view.radio.current.modes == (1u << candidate.security)) {
        view.saving = true;
        page_host_redraw();
        if (store_profile(&candidate)) {
            saved_profile = candidate;
            view.has_saved = true;
            view.message = 0;
        } else view.message = "Connected; not saved";
        attempting = false;
        view.saving = false;
        wifi_secret_clear(&candidate, sizeof(candidate));
    } else if (attempting &&
               ((!standby_since && time_us() >= attempt_deadline) ||
                view.radio.state == WIFI_RADIO_FAILED ||
                (view.radio.request_id == candidate_id && view.radio.attempt_failed))) {
        attempting = false;
        wifi_secret_clear(&candidate, sizeof(candidate));
        backend.connect(view.has_saved ? &saved_profile : 0);
        view.message = view.has_saved ? "Could not connect" : "Could not connect; check password";
    }
    view.saved = view.has_saved ? profile_name(&saved_profile) : (struct wifi_network){0};
    view.storage = fs_persistent_ready();
    view.busy = false;
    working = false;
    /* Only connection state and visible settings trigger a screen redraw: a
     * stage's message only once it failed. */
    if (view.radio.state != WIFI_RADIO_FAILED) before.radio.message = view.radio.message;
    if (memcmp(&before, &view, offsetof(struct wifi_setup_view, radio.networks)))
        page_host_redraw();
}
