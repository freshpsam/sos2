/* The network device over a board's Wi-Fi radio (include/wifi_device.h).
 * The station's frames and steps, the scan list, the desktop's Wi-Fi setup
 * and the network's randomness are the same on every board; the board's
 * struct wifi_radio starts the hardware and runs it. */
#include "wifi_device.h"
#include "network.h"
#include "wifi_setup.h"
#include "wifi_auth.h"
#include "runtime_files.h"
#include "platform.h"
#include "entropy.h"
#include "kernel_io.h"
#include "wifi/port.h"
#include "mbedtls/hmac_drbg.h"
#include <string.h>

static const struct wifi_radio *radio;
static struct wifi_device_view state;
static struct wifi_station station;
static struct wifi_profile profile;
static bool station_started, linked, servicing, rng_ready;
static bool restart_requested, survey_requested, survey_resume;
static volatile bool standby_wanted;
static unsigned request_id, running_id, network_count, scan_generation;
static struct wifi_network networks[WIFI_NETWORK_MAX];
static bool scan_truncated;
static struct net_device *network_device;
static const char *failure_reason;
static struct mbedtls_hmac_drbg_context auth_random;
static uint64_t next_service;

/* Frames from the radio's worker to lwIP on the main loop. A radio poll
 * delivers a whole aggregate at once, up to 64 frames, before lwIP runs:
 * a shorter queue drops the rest, and TCP then resends whole windows. */
enum { RECEIVE_QUEUE = 256 };

static struct {
    unsigned size;
    unsigned char data[1514];
} received[RECEIVE_QUEUE] __attribute__((section(".cpu_private")));

static unsigned receive_head, receive_count;
static unsigned char random_pool[512];
static unsigned random_head;
static bool random_wanted;

void wifi_device_fail(const char *why) {
    if (state.failed) return;
    state.failed = true;
    linked = false;
    state.stopping = state.enabled;
    failure_reason = why;
    /* Teardown runs on the serialized worker after any suspended crypto
     * operation returns, never from a main-loop network callback. */
}

static bool online(void) { return state.enabled && !state.failed && radio->ready(); }

static bool link(void) { return online() && !restart_requested && station.state == WIFI_ONLINE; }

static void stop(void) { wifi_device_fail("network stopped"); }

static bool send_frame(const void *data, unsigned size) {
    if (servicing || !link() || !wifi_station_send(&station, data, size)) return false;
    ++network_device->tx_packets;
    return true;
}

static int configure(void *p, enum wifi_action action, const struct wifi_station *s) {
    (void)p;
    return radio->configure(action, s, time_us());
}

static bool send_wire(void *p, const void *data, unsigned size, bool is_data) {
    (void)p;
    return radio->send(data, size, is_data);
}

static void ethernet(void *p, const void *data, unsigned size) {
    (void)p;
    if (size > 1514 || receive_count == RECEIVE_QUEUE) return;
    unsigned at = (receive_head + receive_count) % RECEIVE_QUEUE;
    memcpy(received[at].data, data, size);
    received[at].size = size;
    ++receive_count;
}

void wifi_device_receive(const unsigned char *frame, unsigned size,
                         const struct wifi_rx_status *status) {
    wifi_station_receive_checked(&station, frame, size, status, time_us());
}

static bool drained(void *p) {
    (void)p;
    return radio->drained();
}

static uint64_t clock_now(void *p) {
    (void)p;
    return time_us();
}

static void discovered(void *p, const struct wifi_network *network) {
    (void)p;
    for (unsigned i = 0; i < network_count; ++i)
        if (networks[i].ssid_length == network->ssid_length &&
            networks[i].modes == network->modes &&
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
    if (!rng_ready || !servicing || !entropy_available() ||
        mbedtls_hmac_drbg_random(&auth_random, out, size))
        return -1;
    kernel_io_wait();
    return state.failed ? -1 : 0;
}

static void poll(void (*receive)(const void *, unsigned)) {
    /* lwIP stays on the main loop. Only bounded copies cross from the
     * worker, so expensive SAE and entropy work cannot freeze the desktop. */
    while (receive_count) {
        if (link()) {
            ++network_device->rx_packets;
            receive(received[receive_head].data, received[receive_head].size);
        }
        received[receive_head].size = 0;
        receive_head = (receive_head + 1) % RECEIVE_QUEUE;
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
        wifi_device_fail("wireless profile unsupported");
        return false;
    }
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

struct wifi_station *wifi_device_station(void) { return &station; }

const unsigned char *wifi_device_mac(void) { return profile.mac; }

bool wifi_device_seed(void) {
    if (!profile.security || rng_ready) return true;
    if (!entropy_available() ||
        mbedtls_hmac_drbg_seed(&auth_random, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                               seed_random, 0, profile.mac, 6)) {
        wifi_device_fail("wireless secure randomness unavailable");
        return false;
    }
    mbedtls_hmac_drbg_set_reseed_interval(&auth_random, 256);
    rng_ready = true;
    return true;
}

bool wifi_device_begin(uint64_t now) {
    if (station_started) return true;
    /* Idle until Scan Networks is tapped or a network is chosen; a chosen
     * network still runs its directed scan. */
    if (!restart_requested) return false;
    if (!survey_requested && !profile.ssid_length) {
        restart_requested = false;
        return false;
    }
    bool survey = survey_requested;
    begin(survey);
    return start_station(now, survey);
}

void wifi_device_poll(void) {
    if (restart_requested) {
        if (station.state == WIFI_BACKOFF || station.state == WIFI_IDLE) {
            /* The old station and BSS are deleted and the radio drained
             * before any profile or authentication state changes. */
            if (!survey_requested && !profile.ssid_length) {
                /* Forgotten, or failed with no saved network: the station's
                 * copy of the old key goes too. */
                wifi_secret_clear(&station.profile, sizeof(station.profile));
                restart_requested = survey_resume = station_started = false;
                station.state = WIFI_IDLE;
                return;
            }
            bool survey = survey_requested;
            begin(survey);
            if (!start_station(time_us(), survey)) return;
        } else wifi_station_reconnect(&station, "network selection changed", time_us());
    } else if (station.survey && station.state == WIFI_IDLE && survey_resume) {
        survey_resume = false;
        if (!start_station(time_us(), false)) return;
    }
    wifi_station_standby(&station, standby_wanted, time_us());
    wifi_station_poll(&station, time_us());
    if (station.state == WIFI_FAILED) {
        wifi_device_fail(station.error);
        return;
    }
    if (!linked && link()) linked = true;
    else if (linked && !link()) {
        linked = false;
        receive_head = receive_count = 0;
    }
}

bool wifi_device_quiet(void) {
    return standby_wanted && !restart_requested && !survey_requested && radio->drained() &&
           (!station_started || ((station.state == WIFI_IDLE || station.state == WIFI_BACKOFF) &&
                                 !station.configuring));
}

static bool start_radio(const unsigned char mac[6]) {
    if (state.enabled || state.failed || !wifi_mac_valid(mac)) return false;
    memcpy(profile.mac, mac, 6);
    memcpy(network_device->mac, mac, 6);
    const char *why = radio->start(mac);
    if (why) {
        failure_reason = why;
        return false;
    }
    state.enabled = true;
    return true;
}

static unsigned connect_profile(const struct wifi_profile *next) {
    if (!state.enabled || state.failed) return 0;
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
    if (state.enabled && !state.failed) {
        survey_requested = restart_requested = true;
        receive_head = receive_count = 0;
    }
}

static void snapshot(struct wifi_radio_view *out) {
    /* Every field before the network list, then the networks found. */
    memset(out, 0, offsetof(struct wifi_radio_view, networks));
    out->message = wifi_device_status();
    out->count = network_count;
    out->scan_generation = scan_generation;
    out->scan_truncated = scan_truncated;
    out->request_id = running_id;
    out->startup_pending =
        state.stopping || (state.enabled && !state.failed && !state.startup_complete);
    memcpy(out->networks, networks, network_count * sizeof(*networks));
    if (state.failed) out->state = WIFI_RADIO_FAILED;
    else if (!state.enabled) out->state = WIFI_RADIO_UNAVAILABLE;
    else if (!station_started) out->state = radio->ready() ? WIFI_RADIO_IDLE : WIFI_RADIO_STARTING;
    else if (restart_requested || station.survey)
        out->state = station.state == WIFI_IDLE && !restart_requested ? WIFI_RADIO_IDLE
                                                                      : WIFI_RADIO_SCANNING;
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

static bool running(void) { return state.enabled && !state.failed; }

static bool service_pending(void) {
    return state.stopping || (running() && loop_time_us() >= next_service) ||
           (running() && random_wanted && state.random_bytes < 256 && entropy_available());
}

static void service(void *unused) {
    (void)unused;
    if (state.stopping) {
        if (radio->stop) radio->stop();
        wifi_station_stop(&station, failure_reason);
        mbedtls_hmac_drbg_free(&auth_random);
        rng_ready = false;
        wifi_crypto_wipe(random_pool, sizeof(random_pool));
        state.random_bytes = 0;
        state.stopping = false;
        return;
    }
    if (running() && time_us() >= next_service) {
        servicing = true;
        uint64_t delay = radio->service();
        next_service = time_us() + delay;
        servicing = false;
    }
    if (running() && random_wanted && state.random_bytes < 256 && entropy_available()) {
        unsigned char bytes[256];
        size_t n = entropy_read(bytes, sizeof(bytes));
        for (unsigned i = 0; i < n; ++i)
            random_pool[(random_head + state.random_bytes + i) % sizeof(random_pool)] = bytes[i];
        state.random_bytes += n;
        wifi_crypto_wipe(bytes, sizeof(bytes));
        if (!n && !entropy_available()) wifi_device_fail("network entropy failed");
    }
    /* Fill both network queues before the desktop shows: otherwise lwIP's
     * first polls trigger several slow jitter collections just after boot. */
    if (radio->ready() && state.random_bytes >= 256 && !network_startup_pending())
        state.startup_complete = true;
}

static void cooperate(void *unused) {
    (void)unused;
    kernel_io_wait();
}

void wifi_device_init(const struct wifi_radio *r, struct net_device *out) {
    radio = r;
    state = (struct wifi_device_view){0};
    station_started = linked = servicing = rng_ready = false;
    restart_requested = survey_requested = survey_resume = false;
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
    random_head = 0;
    random_wanted = false;
    *out = (struct net_device){
        .online = online, .link = link, .poll = poll, .send = send_frame, .stop = stop};
    const struct runtime_file *config = runtime_file_find("/system/wifi-profile");
    struct wifi_profile bootstrap;
    bool imported =
        config && wifi_profile_read(&bootstrap, runtime_file_data(config), config->size);
    wifi_rsn_pmf(!r->no_pmf);
    const struct wifi_setup_ops ops = {
        start_radio,     service_pending,     service, snapshot, scan_networks,
        connect_profile, wifi_auth_credential};
    wifi_setup_init(&ops, imported ? &bootstrap : 0);
    wifi_secret_clear(&bootstrap, sizeof(bootstrap));
}

const struct wifi_device_view *wifi_device_view(void) { return &state; }

void wifi_device_standby(bool on) {
    standby_wanted = on;
    if (!on) next_service = 0; /* the radio back at once */
}

size_t wifi_device_random(void *out, size_t size) {
    random_wanted = true;
    if (size > state.random_bytes) size = state.random_bytes;
    for (unsigned i = 0; i < size; ++i)
        ((unsigned char *)out)[i] = random_pool[(random_head + i) % sizeof(random_pool)];
    for (unsigned i = 0; i < size; ++i) random_pool[(random_head + i) % sizeof(random_pool)] = 0;
    random_head = (random_head + size) % sizeof(random_pool);
    state.random_bytes -= size;
    return size;
}

const char *wifi_device_status(void) {
    if (state.failed) return failure_reason ? failure_reason : "Wi-Fi hardware failed";
    if (!state.enabled) {
        if (failure_reason) return failure_reason;
        if (!entropy_available()) return "Wi-Fi blocked: secure randomness unavailable";
        const char *message = wifi_setup_view()->message;
        return message ? message : "Wi-Fi not started";
    }
    if (!radio->ready()) {
        const char *message = radio->starting ? radio->starting() : 0;
        return message ? message : "Wi-Fi starting";
    }
    if (!station_started) return "Wi-Fi ready";
    if (restart_requested) return "Wi-Fi changing network";
    return wifi_station_status(&station);
}
