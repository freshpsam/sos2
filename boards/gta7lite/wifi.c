/* Wi-Fi on the Galaxy Tab A7 Lite: the connectivity subsystem (connsys.c)
 * and MediaTek's Wi-Fi firmware (wifi_radio.c) under the shared station
 * (kernel/wifi), as on the Redmi 2 (boards/redmi2/protected_devices.c): the
 * desktop's Wi-Fi setup chooses networks and keeps credentials, the station
 * builds and parses 802.11 and runs WPA2/WPA3, and lwIP stays on the main
 * loop. Hardware start and the radio run on the serialized service worker. */
#include "gta7lite.h"
#include "wifi_radio.h"
#include "network.h"
#include "net_device.h"
#include "wifi_setup.h"
#include "runtime_files.h"
#include "platform.h"
#include "entropy.h"
#include "kernel_io.h"
#include "wifi/port.h"
#include "mbedtls/hmac_drbg.h"
#include <string.h>

static struct wifi_station station;
static struct wifi_profile profile;
static bool enabled, failed, station_started, stop_pending, linked, servicing, rng_ready;
static bool restart_requested, survey_requested, survey_resume, radio_ready, startup_complete;
static volatile bool standby_wanted;
static unsigned request_id, running_id, network_count, scan_generation;
static struct wifi_network networks[WIFI_NETWORK_MAX];
static bool scan_truncated;
static struct net_device *network_device;
static const char *failure_reason;
static struct mbedtls_hmac_drbg_context auth_random;
static uint64_t next_service;

static struct {
    unsigned size;
    unsigned char data[1514];
} received[8];

static unsigned receive_head, receive_count;
static unsigned char random_pool[512];
static unsigned random_head, random_count;
static bool random_wanted;

static const char *wifi_status(void);

static void fail(const char *why) {
    if (failed) return;
    failed = true;
    linked = false;
    stop_pending = enabled;
    failure_reason = why;
}

static bool online(void) { return enabled && !failed && radio_ready; }

static bool link(void) { return online() && !restart_requested && station.state == WIFI_ONLINE; }

static void stop(void) { fail("network stopped"); }

static bool send_frame(const void *data, unsigned size) {
    if (servicing || !link() || !wifi_station_send(&station, data, size)) return false;
    ++network_device->tx_packets;
    return true;
}

static int configure(void *p, enum wifi_action action, const struct wifi_station *s) {
    (void)p;
    return gta7lite_wifi_radio_configure(action, s, time_us());
}

static bool send_wire(void *p, const void *data, unsigned size, bool is_data) {
    (void)p;
    return gta7lite_wifi_radio_send(data, size, is_data);
}

static void ethernet(void *p, const void *data, unsigned size) {
    (void)p;
    if (size > 1514 || receive_count == 8) return;
    unsigned at = (receive_head + receive_count) % 8;
    memcpy(received[at].data, data, size);
    received[at].size = size;
    ++receive_count;
}

static void receive_wire(const unsigned char *data, unsigned size, const struct wifi_rx_status *status) {
    wifi_station_receive_checked(&station, data, size, status, time_us());
}

static bool drained(void *p) {
    (void)p;
    return gta7lite_wifi_radio_drained();
}

static uint64_t clock_now(void *p) {
    (void)p;
    return time_us();
}

static void discovered(void *p, const struct wifi_network *network) {
    (void)p;
    for (unsigned i = 0; i < network_count; ++i)
        if (networks[i].ssid_length == network->ssid_length && networks[i].modes == network->modes &&
            !memcmp(networks[i].ssid, network->ssid, network->ssid_length))
            return;
    if (network_count < WIFI_NETWORK_MAX) networks[network_count++] = *network;
    else scan_truncated = true;
}

static int seed_random(void *p, unsigned char *out, size_t size) {
    (void)p;
    return entropy_read(out, size) == size ? 0 : -1;
}

static int authentication_random(void *p, unsigned char *out, size_t size) {
    (void)p;
    if (!rng_ready || !servicing || !entropy_available() || mbedtls_hmac_drbg_random(&auth_random, out, size))
        return -1;
    kernel_io_wait();
    return failed ? -1 : 0;
}

static void poll(void (*receive)(const void *, unsigned)) {
    /* lwIP stays on the main loop; only bounded copies cross from the worker. */
    while (receive_count) {
        if (link()) {
            ++network_device->rx_packets;
            receive(received[receive_head].data, received[receive_head].size);
        }
        received[receive_head].size = 0;
        receive_head = (receive_head + 1) % 8;
        --receive_count;
    }
}

static bool start_station(uint64_t now, bool survey) {
    struct wifi_ops ops = {.configure = configure,
                           .send = send_wire,
                           .receive = ethernet,
                           .random = authentication_random,
                           .drained = drained,
                           .clock = clock_now,
                           .discovered = discovered};
    bool result = survey ? wifi_station_survey(&station, profile.mac, &ops, now)
                         : wifi_station_start(&station, &profile, &ops, now);
    if (!result) {
        fail("wireless profile unsupported");
        return false;
    }
    gta7lite_wifi_radio_station(&station);
    station_started = true;
    running_id = request_id;
    restart_requested = false;
    return true;
}

static void begin(bool survey) {
    survey_resume = survey && profile.ssid_length;
    survey_requested = false;
    if (survey) {
        network_count = 0;
        scan_truncated = false;
        ++scan_generation;
    }
}

static void radio_poll(void) {
    uint64_t now = time_us();
    if (profile.security && !rng_ready) {
        if (!entropy_available() ||
            mbedtls_hmac_drbg_seed(&auth_random, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), seed_random, 0,
                                   profile.mac, 6)) {
            fail("wireless secure randomness unavailable");
            return;
        }
        mbedtls_hmac_drbg_set_reseed_interval(&auth_random, 256);
        rng_ready = true;
        now = time_us();
    }
    if (!station_started) {
        /* Idle until Scan Networks is tapped or a network is chosen. */
        if (!restart_requested) return;
        if (!survey_requested && !profile.ssid_length) {
            restart_requested = false;
            return;
        }
        bool survey = survey_requested;
        begin(survey);
        if (!start_station(now, survey)) return;
    }
    if (gta7lite_wifi_radio_beacon_lost()) wifi_station_beacon_missed(&station, now);
    gta7lite_wifi_radio_poll(receive_wire);
    if (restart_requested) {
        if (station.state == WIFI_BACKOFF || station.state == WIFI_IDLE) {
            /* The old station and BSS are deleted and the rings drained
             * before any profile or authentication state changes. */
            if (!survey_requested && !profile.ssid_length) {
                wifi_secret_clear(&station.profile, sizeof(station.profile));
                restart_requested = survey_resume = station_started = false;
                station.state = WIFI_IDLE;
                return;
            }
            bool survey = survey_requested;
            begin(survey);
            if (!start_station(time_us(), survey)) return;
        } else
            wifi_station_reconnect(&station, "network selection changed", time_us());
    } else if (station.survey && station.state == WIFI_IDLE && survey_resume) {
        survey_resume = false;
        if (!start_station(time_us(), false)) return;
    }
    wifi_station_standby(&station, standby_wanted, time_us());
    wifi_station_poll(&station, time_us());
    if (station.state == WIFI_FAILED) {
        fail(station.error);
        return;
    }
    if (!linked && link()) linked = true;
    else if (linked && !link()) {
        linked = false;
        receive_head = receive_count = 0;
    }
}

/* In standby, once the station has left its network or has none, the chip
 * goes to its firmware, which sleeps it, as the phone's radio idles; awake
 * otherwise. False while it sleeps: nothing polls it then. */
static bool radio_awake(void) {
    bool sleep = standby_wanted && !restart_requested && !survey_requested && gta7lite_wifi_radio_drained() &&
                 (!station_started ||
                  ((station.state == WIFI_IDLE || station.state == WIFI_BACKOFF) && !station.configuring));
    int state = gta7lite_wifi_radio_sleep(sleep);
    if (state < 0) fail("Wi-Fi did not wake");
    return !state;
}

static void cooperate(void *unused) {
    (void)unused;
    kernel_io_wait();
}

static void yield(void) { kernel_io_wait(); }

static bool start_radio(const unsigned char mac[6]) {
    if (enabled || failed || !wifi_mac_valid(mac)) return false;
    if (!gta7lite_connsys_reserved()) {
        failure_reason = "Wi-Fi memory unavailable";
        return false;
    }
    memcpy(profile.mac, mac, 6);
    memcpy(network_device->mac, mac, 6);
    enabled = true;
    return true;
}

static unsigned connect_profile(const struct wifi_profile *next) {
    if (!enabled || failed) return 0;
    if (next) {
        unsigned char bytes[WIFI_PROFILE_BYTES];
        bool valid = wifi_profile_write(next, bytes);
        wifi_secret_clear(bytes, sizeof(bytes));
        if (!valid) return 0;
    }
    wifi_secret_clear(&profile, sizeof(profile));
    if (next) profile = *next;
    memcpy(profile.mac, network_device->mac, 6);
    if (!++request_id) ++request_id;
    restart_requested = true;
    survey_requested = survey_resume = false;
    receive_head = receive_count = 0;
    return request_id;
}

static void scan_networks(void) {
    if (enabled && !failed) {
        survey_requested = restart_requested = true;
        receive_head = receive_count = 0;
    }
}

static void snapshot(struct wifi_radio_view *out) {
    /* Every field before the network list, then the networks found. */
    memset(out, 0, offsetof(struct wifi_radio_view, networks));
    out->message = wifi_status();
    out->count = network_count;
    out->scan_generation = scan_generation;
    out->scan_truncated = scan_truncated;
    out->request_id = running_id;
    out->startup_pending = stop_pending || (enabled && !failed && !startup_complete);
    memcpy(out->networks, networks, network_count * sizeof(*networks));
    if (failed) out->state = WIFI_RADIO_FAILED;
    else if (!enabled) out->state = WIFI_RADIO_UNAVAILABLE;
    else if (!station_started) out->state = radio_ready ? WIFI_RADIO_IDLE : WIFI_RADIO_STARTING;
    else if (restart_requested || station.survey)
        out->state = station.state == WIFI_IDLE && !restart_requested ? WIFI_RADIO_IDLE : WIFI_RADIO_SCANNING;
    else if (link()) out->state = WIFI_RADIO_CONNECTED;
    else out->state = WIFI_RADIO_CONNECTING;
    out->attempt_failed = !restart_requested && !station.survey && !station.leaving &&
                          (station.state == WIFI_RECOVER || station.state == WIFI_BACKOFF);
    if (station_started && !station.survey) {
        out->current.ssid_length = station.profile.ssid_length;
        memcpy(out->current.ssid, station.profile.ssid, station.profile.ssid_length);
        out->current.modes = 1u << station.profile.security;
    }
}

static bool wifi_service_pending(void) {
    return stop_pending || (enabled && !failed && time_us() >= next_service) ||
           (enabled && !failed && random_wanted && random_count < 256 && entropy_available());
}

static void wifi_service(void *unused) {
    (void)unused;
    if (stop_pending) {
        wifi_station_stop(&station, failure_reason);
        mbedtls_hmac_drbg_free(&auth_random);
        rng_ready = false;
        wifi_crypto_wipe(random_pool, sizeof(random_pool));
        random_count = 0;
        stop_pending = false;
        return;
    }
    if (enabled && !failed && time_us() >= next_service) {
        servicing = true;
        bool awake = true;
        if (!radio_ready) {
            const char *error = gta7lite_connsys_start(yield);
            if (!error) error = gta7lite_wifi_radio_start(profile.mac, yield);
            if (error) fail(error);
            else radio_ready = true;
        } else if ((awake = radio_awake()))
            radio_poll();
        next_service = time_us() + (awake ? 2000 : 250000);
        servicing = false;
    }
    if (enabled && !failed && random_wanted && random_count < 256 && entropy_available()) {
        unsigned char bytes[256];
        size_t n = entropy_read(bytes, sizeof(bytes));
        for (unsigned i = 0; i < n; ++i)
            random_pool[(random_head + random_count + i) % sizeof(random_pool)] = bytes[i];
        random_count += n;
        volatile unsigned char *wipe = bytes;
        for (unsigned i = 0; i < sizeof(bytes); ++i) wipe[i] = 0;
        if (!n && !entropy_available()) fail("network entropy failed");
    }
    if (radio_ready && random_count >= 256 && !network_startup_pending()) startup_complete = true;
}

void gta7lite_wifi_standby(bool on) {
    standby_wanted = on;
    if (!on) next_service = 0; /* the chip back at once */
}

size_t gta7lite_wifi_random(void *out, size_t size) {
    random_wanted = true;
    if (size > random_count) size = random_count;
    for (unsigned i = 0; i < size; ++i) ((unsigned char *)out)[i] = random_pool[(random_head + i) % sizeof(random_pool)];
    for (unsigned i = 0; i < size; ++i) random_pool[(random_head + i) % sizeof(random_pool)] = 0;
    random_head = (random_head + size) % sizeof(random_pool);
    random_count -= size;
    return size;
}

static const char *wifi_status(void) {
    if (failed) return failure_reason ? failure_reason : "Wi-Fi hardware failed";
    if (!enabled) {
        if (failure_reason) return failure_reason;
        if (!entropy_available()) return "Wi-Fi blocked: secure randomness unavailable";
        const char *message = wifi_setup_view()->message;
        return message ? message : "Wi-Fi not started";
    }
    if (!radio_ready) return "Wi-Fi starting";
    if (!station_started) return "Wi-Fi ready";
    if (restart_requested) return "Wi-Fi changing network";
    return wifi_station_status(&station);
}

bool gta7lite_wifi_init(const struct platform_devices *devices, struct net_device *out) {
    (void)devices;
    enabled = failed = station_started = stop_pending = linked = servicing = rng_ready = false;
    restart_requested = survey_requested = survey_resume = radio_ready = startup_complete = false;
    request_id = running_id = network_count = scan_generation = 0;
    scan_truncated = false;
    next_service = 0;
    network_device = out;
    receive_head = receive_count = 0;
    failure_reason = 0;
    wifi_port_reset();
    wifi_port_set_yield(cooperate, 0);
    mbedtls_hmac_drbg_init(&auth_random);
    profile = (struct wifi_profile){0};
    random_head = random_count = 0;
    random_wanted = false;
    *out = (struct net_device){.online = online, .link = link, .poll = poll, .send = send_frame, .stop = stop};
    const struct runtime_file *config = runtime_file_find("/system/wifi-profile");
    struct wifi_profile bootstrap;
    bool imported = config && wifi_profile_read(&bootstrap, runtime_file_data(config), config->size);
    const struct wifi_setup_ops ops = {start_radio,     wifi_service_pending, wifi_service, snapshot,
                                       scan_networks,   connect_profile,               wifi_auth_credential};
    wifi_setup_init(&ops, imported ? &bootstrap : 0);
    wifi_secret_clear(&bootstrap, sizeof(bootstrap));
    return true;
}
