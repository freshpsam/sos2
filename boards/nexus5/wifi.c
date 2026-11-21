#include "nexus5.h"
#include "kernel_io.h"
#include "platform.h"
#include "wifi_device.h"
#include "wire.h"
#include "wifi/port.h"

/* Wi-Fi on the Nexus 5: the Broadcom BCM4339 on SDIO (sdio.c), its
 * firmware (bcm4339.c) as the radio of the shared Wi-Fi device
 * (kernel/wifi/wifi_device.c).
 * Its power, as Linux's qcom-msm8974-lge-nexus5-hammerhead.dts gives it:
 * the chip's 3.3 V supply switched by TLMM GPIO 26 (vreg_wlan, which also
 * holds the chip out of reset), the 32 kHz sleep clock out of PM8941
 * GPIO16 (its second special function, an output on S3's 1.8 V), and
 * PM8941 GPIO17 a plain input beside it (wl-reg-active-state).
 * The firmware is FullMAC: it scans, authenticates and associates itself,
 * where the shared station (kernel/wifi/wifi_station.c) builds and parses
 * those 802.11 frames for a SoftMAC radio. So this radio gives the station
 * the firmware's news through its FullMAC calls (wifi_station.h), as
 * Linux's brcmfmac drives this firmware with a host supplicant
 * (cfg80211.c): a channel's scan is an escan there, each result handed on
 * as the beacon it describes; the station's authentication is answered at
 * once (open system); its association request becomes the firmware's
 * join, with the station's RSN element as the firmware's own, and the
 * station associated when the firmware reports the network joined; WPA2's
 * handshake is EAPOL in data frames, and its keys go to the firmware; data
 * moves as Ethernet, converted to and from 802.11 data frames; a link lost
 * or a deauthentication reported disconnects the station, and its probes of
 * a quiet network count as the network heard while the firmware holds the
 * link. Frames and news reach the station in the order they came. The
 * firmware has no management frame protection, so the station never
 * negotiates it (no_pmf). In standby, once the station has left its network
 * or has none, the firmware takes its radio down, as for an interface
 * brought down, and nothing polls the chip until standby ends. A failure
 * turns the chip's supply off. */

enum {
    REG_ON = 26,
    PIN_OUTPUT = 1 << 9,
    PIN_OUT = 2,
    /* PM8941 GPIOs (slave 0, 0xc000 + 0x100 (n - 1); the stock kernel's
     * qpnp-pin.c): mode (input 0 or output 1 in 6:4, source in 3:1: 0 a
     * constant, 3 the second special function; 0 the output's value),
     * supply (2: S3), pull (5: none), enable. */
    SLEEP_CLOCK = 0xcf00,
    BESIDE = 0xd000,
    MODE = 0x40,
    SUPPLY = 0x41,
    PULL = 0x42,
    ENABLE = 0x46,
    POWER_UP_MS = 200,
    /* Broadcom ioctls (brcmfmac fwil.h). */
    GET_VERSION = 1,
    UP = 2,
    DOWN = 3,
    SET_INFRA = 20,
    SET_SSID = 26,
    SET_KEY = 45,
    SCAN = 50,
    DISASSOC = 52,
    SET_PM = 86,
    PM_FAST = 2,
    /* Events (brcmfmac fweh.h). */
    E_SET_SSID = 0,
    E_DEAUTH = 5,
    E_DEAUTH_IND = 6,
    E_DISASSOC_IND = 12,
    E_LINK = 16,
    E_ESCAN_RESULT = 69,
    ESCAN_PARTIAL = 8,
    /* Frames and news waiting for the station, each frame at most FRAME
     * bytes: a poll reads no more than fit, and the shared device takes up
     * to 64 a step. */
    QUEUE = 64,
    FRAME = 2048,
    EAPOL_GUARD_US = 50000,
    SCAN_END_US = 1000000,
};

static bool pmic_gpio(uint16_t base, uint8_t mode, bool output) {
    return nexus5_pmic_write(base + SUPPLY, 2) && (output || nexus5_pmic_write(base + PULL, 5)) &&
           nexus5_pmic_write(base + MODE, mode) && nexus5_pmic_write(base + ENABLE, 0x80);
}

const char *nexus5_wifi_power(void) {
    if (!pmic_gpio(SLEEP_CLOCK, 1u << 4 | 3u << 1 | 1, true) || !pmic_gpio(BESIDE, 0, false))
        return "Wi-Fi sleep clock unavailable";
    /* Off first, so the chip starts from reset. */
    mmio_write32(nexus5_gpio(REG_ON) + 4, 0);
    mmio_write32(nexus5_gpio(REG_ON), PIN_OUTPUT);
    delay_us(10000);
    mmio_write32(nexus5_gpio(REG_ON) + 4, PIN_OUT);
    for (uint64_t start = time_us(); time_us() - start < POWER_UP_MS * 1000ull;) kernel_io_wait();
    return 0;
}

static bool radio_ready, radio_down;
static unsigned char mac[6];
static uint32_t io_type; /* chanspec format: 1 802.11n, 2 802.11ac */

/* What waits for the station: a frame, or the firmware's news. */
enum entry { FRAME_ENTRY, AUTHENTICATED, ASSOCIATED, DISCONNECTED, HEARD };

static struct {
    enum entry entry;
    unsigned length;
    uint16_t reason; /* a disconnection's */
    struct wifi_rx_status status;
    uint8_t data[FRAME];
} queue[QUEUE];

static unsigned queue_head, queue_count;
static uint16_t sequence;

/* The station's view of the network being joined; deauth_waiting: a
 * disconnection the queue had no room for yet. */
static struct {
    bool joining, associated, pairwise, deauth_waiting;
    uint16_t deauth_reason;
    int group; /* the group key's index, -1 without */
    unsigned char bssid[6];
    uint64_t last_eapol;
} link;

static struct {
    bool running, abort_sent;
    uint64_t started;
} scan;

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static unsigned room(void) { return QUEUE - queue_count; }

static uint8_t *enqueue(unsigned length, const struct wifi_rx_status *status) {
    if (length > FRAME || queue_count == QUEUE) return 0;
    unsigned slot = (queue_head + queue_count++) % QUEUE;
    queue[slot].entry = FRAME_ENTRY;
    queue[slot].length = length;
    queue[slot].status = *status;
    return queue[slot].data;
}

/* The firmware's news, in line with the frames: false when the queue is full. */
static bool tell(enum entry entry, uint16_t reason) {
    if (queue_count == QUEUE) return false;
    unsigned slot = (queue_head + queue_count++) % QUEUE;
    queue[slot].entry = entry;
    queue[slot].reason = reason;
    return true;
}

static const struct wifi_rx_status management = {.management = true};

/* An 802.11 management frame's header from the network to the station. */
static void header(uint8_t *f, unsigned type, const unsigned char *to, const unsigned char *bssid) {
    memset(f, 0, 24);
    put16(f, (uint16_t)type);
    memcpy(f + 4, to, 6);
    memcpy(f + 10, bssid, 6);
    memcpy(f + 16, bssid, 6);
    put16(f + 22, (uint16_t)(sequence++ << 4));
}

/* An escan result (brcmf_bss_info_le) as the beacon it describes: BSSID,
 * beacon period, capability and the information elements. */
static void scanned(const uint8_t *bss, unsigned size) {
    if (size < 128) return;
    unsigned length = le32(bss + 4), offset = le16(bss + 116), ies = le32(bss + 120);
    if (length > size || offset > length || ies > length - offset || 36 + ies > FRAME) return;
    static const unsigned char broadcast[6] = {255, 255, 255, 255, 255, 255};
    uint8_t *f = enqueue(36 + ies, &management);
    if (!f) return;
    header(f, 0x80, broadcast, bss + 8);
    memset(f + 24, 0, 8);
    put16(f + 32, le16(bss + 14));
    put16(f + 34, le16(bss + 16));
    memcpy(f + 36, bss + offset, ies);
}

static void queue_deauth(void) {
    if (tell(DISCONNECTED, link.deauth_reason)) link.deauth_waiting = false;
}

/* The link gone: the station told now, or as soon as the queue has room. */
static void deauthenticate(unsigned reason) {
    if (!link.associated) return;
    link.associated = link.joining = link.pairwise = false;
    link.group = -1;
    link.deauth_waiting = true;
    link.deauth_reason = (uint16_t)reason;
    queue_deauth();
}

/* The station associated: lost with a full queue, its association request
 * comes again and is answered at once while the firmware holds the link. */
static void answer_association(void) { (void)tell(ASSOCIATED, 0); }

static void event(const uint8_t *m, unsigned length) {
    static const uint8_t oui[3] = {0x00, 0x10, 0x18};
    if (length < 72 || be16(m + 12) != 0x886c || memcmp(m + 19, oui, 3) || be16(m + 22) != 1)
        return;
    uint32_t type = be32(m + 28), status = be32(m + 32), reason = be32(m + 36), data = be32(m + 44);
    uint16_t flags = be16(m + 26);
    if (data > length - 72) return;
    switch (type) {
        case E_ESCAN_RESULT:
            if (status == ESCAN_PARTIAL) {
                if (data >= 12 + 128 && le16(m + 72 + 10) == 1) scanned(m + 72 + 12, data - 12);
            } else scan.running = false;
            break;
        case E_SET_SSID:
            if (!link.joining) break;
            link.joining = false;
            if (status) break; /* the station asks again */
            link.associated = true;
            answer_association();
            break;
        case E_LINK:
            if (!(flags & 1) && link.associated) deauthenticate(0);
            break;
        case E_DEAUTH:
        case E_DEAUTH_IND:
        case E_DISASSOC_IND:
            if (link.associated && !memcmp(m + 48, link.bssid, 6)) deauthenticate(reason);
            break;
    }
}

/* A data frame from the firmware, as Ethernet, made the FromDS 802.11 frame
 * the station expects: protected once the matching key is in. */
static void frame(const uint8_t *e, unsigned length) {
    if (length < 14 || !link.associated) return;
    bool group = e[0] & 1, protect = group ? link.group >= 0 : link.pairwise;
    struct wifi_rx_status status = {.decrypted = protect,
                                    .key_index =
                                        group && link.group >= 0 ? (unsigned)link.group : 0,
                                    .data_offset = 24};
    uint8_t *f = enqueue(length - 14 + 32, &status);
    if (!f) return;
    memset(f, 0, 24);
    put16(f, (uint16_t)(0x0208 | (protect ? 0x4000 : 0)));
    memcpy(f + 4, e, 6);
    memcpy(f + 10, link.bssid, 6);
    memcpy(f + 16, e + 6, 6);
    put16(f + 22, (uint16_t)(sequence++ << 4));
    static const uint8_t snap[6] = {0xaa, 0xaa, 0x03, 0, 0, 0};
    memcpy(f + 24, snap, 6);
    memcpy(f + 30, e + 12, length - 12);
}

static uint16_t chanspec(unsigned channel) {
    return (uint16_t)((io_type == 1 ? 0x2b00 : 0x1000) | channel);
}

/* An escan of one 2.4 GHz channel, probing for the station's network (or
 * any) (brcmfmac's escan_prep, v1 parameters). */
static bool start_scan(const struct wifi_station *s) {
    uint8_t p[112];
    memset(p, 0, sizeof p);
    put32(p, 1);
    put16(p + 4, 1);
    put16(p + 6, 0x1234);
    memset(p + 44, 0xff, 6);
    p[50] = 2;
    p[51] = 0;
    for (unsigned at = 52; at < 68; at += 4) put32(p + at, 0xffffffffu);
    put32(p + 68, 1u << 16 | 1);
    put16(p + 72, chanspec(s->channel));
    if (!s->survey && s->profile.ssid_length) {
        put32(p + 76, s->profile.ssid_length);
        memcpy(p + 80, s->profile.ssid, s->profile.ssid_length);
    }
    scan.running = true;
    scan.abort_sent = false;
    scan.started = time_us();
    return !nexus5_bcm_iovar("escan", true, p, sizeof p);
}

static void abort_scan(void) {
    uint8_t p[68];
    memset(p, 0, sizeof p);
    memset(p + 36, 0xff, 6);
    p[42] = 2;
    for (unsigned at = 44; at < 60; at += 4) put32(p + at, 0xffffffffu);
    put32(p + 60, 1);
    put16(p + 64, 0xffff);
    (void)nexus5_bcm_ioctl(SCAN, true, p, sizeof p, 0);
    scan.abort_sent = true;
}

/* The station's association request as the firmware's join (brcmfmac's
 * brcmf_cfg80211_connect): its RSN element, WPA2-PSK or open, CCMP, the
 * firmware's supplicant off, then a join of that BSSID on its channel. */
static bool join(const struct wifi_station *s, const uint8_t *f, unsigned n) {
    const uint8_t *rsn = 0;
    for (unsigned at = 28; at + 2 <= n && at + 2 + f[at + 1] <= n; at += 2u + f[at + 1])
        if (f[at] == 48) rsn = f + at;
    bool secured = s->profile.security != WIFI_OPEN;
    if (secured && !rsn) return false;
    if (nexus5_bcm_iovar("wpaie", true, (void *)rsn, rsn ? 2u + rsn[1] : 0u) ||
        nexus5_bcm_iovar_int("wpa_auth", secured ? 0x80 : 0) || nexus5_bcm_iovar_int("auth", 0) ||
        nexus5_bcm_iovar_int("wsec", secured ? 4 : 0))
        return false;
    (void)nexus5_bcm_iovar_int("sup_wpa", 0);
    uint8_t p[70];
    memset(p, 0, sizeof p);
    put32(p, s->profile.ssid_length);
    memcpy(p + 4, s->profile.ssid, s->profile.ssid_length);
    p[36] = 0xff;
    for (unsigned at = 40; at < 56; at += 4) put32(p + at, 0xffffffffu);
    memcpy(p + 56, s->bssid, 6);
    put32(p + 64, 1);
    put16(p + 68, chanspec(s->channel));
    memcpy(link.bssid, s->bssid, 6);
    link.joining = true;
    link.associated = link.pairwise = link.deauth_waiting = false;
    link.group = -1;
    if (!nexus5_bcm_iovar("join", true, p, sizeof p)) return true;
    /* An older firmware's join: SET_SSID with the BSSID and channel. */
    uint8_t q[52];
    memset(q, 0, sizeof q);
    memcpy(q, p, 36);
    memcpy(q + 36, s->bssid, 6);
    put32(q + 44, 1);
    put16(q + 48, chanspec(s->channel));
    if (!nexus5_bcm_ioctl(SET_SSID, true, q, sizeof q, 0)) return true;
    link.joining = false;
    return false;
}

/* A key into the firmware (brcmf_wsec_key_le): the pairwise key for the
 * access point, or a group key with its receive counter. */
static bool install(const struct wifi_station *s, const struct wifi_key *k) {
    uint8_t key[164];
    memset(key, 0, sizeof key);
    bool pairwise = k->type == WIFI_KEY_PAIRWISE;
    put32(key, pairwise ? 0 : k->index);
    put32(key + 4, 16);
    memcpy(key + 8, k->key, 16);
    put32(key + 112, 4); /* AES-CCM */
    if (pairwise) memcpy(key + 156, s->bssid, 6);
    else {
        const uint8_t *q = k->sequence;
        put32(key + 116, 2); /* primary */
        put32(key + 132, 1);
        put32(key + 140, (uint32_t)q[5] << 24 | (uint32_t)q[4] << 16 | (uint32_t)q[3] << 8 | q[2]);
        put16(key + 144, (uint16_t)(q[1] << 8 | q[0]));
    }
    bool ok = !nexus5_bcm_iovar("wsec_key", true, key, sizeof key) ||
              !nexus5_bcm_ioctl(SET_KEY, true, key, sizeof key, 0);
    wifi_crypto_wipe(key, sizeof key);
    if (!ok) return false;
    if (pairwise) link.pairwise = true;
    else link.group = (int)k->index;
    return true;
}

static int configure(enum wifi_action action, const struct wifi_station *s, uint64_t now) {
    if (!radio_ready) return -1;
    switch (action) {
        case WIFI_SCAN_INIT:
        case WIFI_SCAN_FINISH:
        case WIFI_BSS_ADD:
        case WIFI_LINK:
        case WIFI_BSS_UPDATE:
        case WIFI_STA:
        case WIFI_LINK_IDLE:
        case WIFI_DELETE_STA: return 1;
        case WIFI_SCAN_START: return start_scan(s) ? 1 : -1;
        case WIFI_SCAN_END:
            if (!scan.running) return 1;
            if (!scan.abort_sent) abort_scan();
            if (now - scan.started >= SCAN_END_US) scan.running = false;
            return scan.running ? 0 : 1;
        case WIFI_JOIN:
            /* SAE runs in the station's authentication frames, which this
             * firmware cannot carry (no_pmf keeps WPA3 from being chosen). */
            return s->profile.security == WIFI_WPA3_SAE ? -1 : 1;
        case WIFI_INSTALL_KEY: {
            const struct wifi_key *k = s->install_key;
            if (!k) return -1;
            if (k->type == WIFI_KEY_INTEGRITY) return 1;
            return install(s, k) ? 1 : -1;
        }
        case WIFI_DELETE_BSS:
            if (link.associated || link.joining) (void)nexus5_bcm_ioctl(DISASSOC, true, 0, 0, 0);
            link.associated = link.joining = link.pairwise = link.deauth_waiting = false;
            link.group = -1;
            return 1;
    }
    return -1;
}

/* An 802.11 frame from the station: a data frame goes out as Ethernet;
 * authentication is answered here, association starts the join, and a
 * probe of the joined network counts as the network heard while the
 * firmware holds the link. */
static bool send(const void *frame_data, unsigned size, bool data) {
    const uint8_t *f = frame_data;
    struct wifi_station *s = wifi_device_station();
    if (!radio_ready || !f || size < 24 || !s) return false;
    if (data) {
        static uint8_t e[1514];
        if (size < 32 || size - 32 + 14 > sizeof e) return false;
        memcpy(e, f + 16, 6);
        memcpy(e + 6, f + 10, 6);
        memcpy(e + 12, f + 30, size - 30);
        if (f[30] == 0x88 && f[31] == 0x8e) link.last_eapol = time_us();
        return nexus5_bcm_send(e, size - 32 + 14, 0);
    }
    unsigned type = le16(f) & 0xfc;
    if (type == 0xb0) return tell(AUTHENTICATED, 0);
    if (type == 0x00) {
        if (link.associated && !memcmp(link.bssid, s->bssid, 6)) {
            answer_association();
            return true;
        }
        return link.joining || join(s, f, size);
    }
    if (type == 0x40 && link.associated && !memcmp(link.bssid, s->bssid, 6)) (void)tell(HEARD, 0);
    return true;
}

static bool drained(void) { return time_us() - link.last_eapol >= EAPOL_GUARD_US; }

static bool ready(void) { return radio_ready; }

/* After the firmware starts: no glomming, the station's address, the
 * events this radio reads (as a 16-byte mask), no roaming, infrastructure
 * mode, up, its fast power saving (PM_FAST: the radio sleeps between
 * beacons while the link is idle, wakes for traffic, as Android runs this
 * chip with brcmfmac's default), and the chanspec format. */
static const char *configure_firmware(void) {
    static const uint8_t events[16] = {0xeb, 0x1a, 0x0b, 0, 0, 0x40, 0x40, 0, 0x20};
    uint8_t v[4];
    if (nexus5_bcm_iovar_int("bus:txglom", 0) || nexus5_bcm_iovar("cur_etheraddr", true, mac, 6) ||
        nexus5_bcm_iovar("event_msgs", true, (void *)events, sizeof events))
        return "Wi-Fi firmware rejected its settings";
    (void)nexus5_bcm_iovar_int("mpc", 0);
    (void)nexus5_bcm_iovar_int("roam_off", 1);
    put32(v, 1);
    if (nexus5_bcm_ioctl(SET_INFRA, true, v, 4, 0)) return "Wi-Fi firmware rejected its mode";
    put32(v, 0);
    if (nexus5_bcm_ioctl(UP, true, v, 4, 0)) return "Wi-Fi radio did not come up";
    put32(v, PM_FAST);
    (void)nexus5_bcm_ioctl(SET_PM, true, v, 4, 0);
    if (nexus5_bcm_ioctl(GET_VERSION, false, v, 4, 4)) return "Wi-Fi firmware version unknown";
    io_type = le32(v);
    return 0;
}

/* Unavailable without the firmware, before the chip is powered. */
static const char *start(const unsigned char address[6]) {
    if (!nexus5_bcm_firmware()) return "Wi-Fi firmware missing";
    memcpy(mac, address, 6);
    return 0;
}

/* After a failure: the chip's supply off; the bus and the driver refuse it
 * from then on. */
static void stop(void) {
    radio_ready = false;
    nexus5_bcm_stop();
    nexus5_sdio_stop();
    mmio_write32(nexus5_gpio(REG_ON) + 4, 0);
    barrier();
}

/* Down while the station is quiet in standby (wifi_device_quiet), up again
 * after: false while it is down. */
static bool radio_awake(void) {
    bool quiet = wifi_device_quiet();
    if (quiet != radio_down) {
        uint8_t v[4];
        put32(v, 0);
        if (nexus5_bcm_ioctl(quiet ? DOWN : UP, true, v, 4, 0)) {
            wifi_device_fail(quiet ? "Wi-Fi radio did not go down" : "Wi-Fi radio did not come up");
            return false;
        }
        radio_down = quiet;
    }
    return !radio_down;
}

/* A queue entry to the station: a frame, or the firmware's news. */
static void deliver(unsigned slot) {
    struct wifi_station *s = wifi_device_station();
    switch (queue[slot].entry) {
        case FRAME_ENTRY:
            wifi_device_receive(queue[slot].data, queue[slot].length, &queue[slot].status);
            break;
        case AUTHENTICATED: wifi_station_authenticated(s, time_us()); break;
        case ASSOCIATED: wifi_station_associated(s, time_us()); break;
        case DISCONNECTED: wifi_station_disconnected(s, queue[slot].reason, time_us()); break;
        case HEARD: wifi_station_heard(s, time_us()); break;
    }
}

static uint64_t service(void) {
    if (!radio_ready) {
        const char *error = nexus5_sdio_start();
        if (!error) error = nexus5_bcm_start(event, frame, room);
        if (!error) error = configure_firmware();
        if (error) {
            wifi_device_fail(error);
            return 1000000;
        }
        radio_ready = true;
        radio_down = false;
        queue_head = queue_count = 0;
        link.group = -1;
        return 2000;
    }
    if (!radio_awake()) return 250000;
    if (!wifi_device_seed() || !wifi_device_begin(time_us())) return 20000;
    if (!nexus5_bcm_poll()) {
        wifi_device_fail("Wi-Fi chip stopped answering");
        return 1000000;
    }
    /* Each frame keeps its slot until the station is done with it: the
     * station's sends and ioctls read more frames into the queue meanwhile. */
    while (queue_count || link.deauth_waiting) {
        if (!queue_count) queue_deauth();
        deliver(queue_head);
        queue_head = (queue_head + 1) % QUEUE;
        --queue_count;
    }
    wifi_device_poll();
    return 2000;
}

static const struct wifi_radio radio = {.start = start,
                                        .service = service,
                                        .stop = stop,
                                        .ready = ready,
                                        .configure = configure,
                                        .send = send,
                                        .drained = drained,
                                        .no_pmf = true};

void nexus5_wifi_init(struct net_device *out) {
    radio_ready = false;
    wifi_device_init(&radio, out);
}
