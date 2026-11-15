#include "network_time.h"
#include "system_clock.h"
#include "lwip/dns.h"
#include "lwip/udp.h"

/* A bounded, unicast SNTPv4 client (RFC 5905). This bootstraps UTC, not
 * authenticated time: source/nonce checks prevent unrelated replies but do
 * not authenticate a server or protect against an on-path attacker.
 * https://developers.cloudflare.com/time-services/ntp/usage/ */
#define SECOND    1000000ull
#define NTP_EPOCH 2208988800ull

enum { IDLE, RESOLVING, WAITING };

static char server[254];
static uint16_t server_port;
static struct udp_pcb *socket;
static ip_addr_t remote;
static unsigned phase, generation, retry_seconds;
static bool connected, dns_busy, denied, was_ready;
static uint64_t next_attempt, earliest_request, deadline, sent_at;
static unsigned char nonce[8];
extern uint32_t network_random(void);

static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put32(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (24 - 8 * i));
}

static bool timestamp(const unsigned char *p, uint64_t *us) {
    uint64_t seconds = get32(p);
    if (!seconds && !get32(p + 4)) return false;
    /* The supported 2000--2099 range crosses the NTP era rollover in 2036. */
    if (seconds < NTP_EPOCH + SYSTEM_CLOCK_MIN_SECONDS) seconds += 1ull << 32;
    seconds -= NTP_EPOCH;
    if (seconds < SYSTEM_CLOCK_MIN_SECONDS || seconds >= SYSTEM_CLOCK_END_SECONDS) return false;
    *us = seconds * SECOND + (((uint64_t)get32(p + 4) * SECOND) >> 32);
    return true;
}

static void cancel(void) {
    if (socket) {
        udp_remove(socket);
        socket = 0;
    }
    phase = IDLE;
    ++generation;
    /* An outstanding DNS query owns its callback until lwIP completes it. */
}

static void retry(void) {
    cancel();
    next_attempt = time_us() + (uint64_t)retry_seconds * SECOND;
    if (retry_seconds < 900) {
        retry_seconds *= 2;
        if (retry_seconds > 900) retry_seconds = 900;
    }
    system_clock_network_status(CLOCK_NETWORK_RETRY);
}

static void received(void *unused, struct udp_pcb *pcb, struct pbuf *packet,
                     const ip_addr_t *address, u16_t port) {
    (void)unused;
    unsigned char data[48];
    uint64_t now = time_us();
    bool matches = phase == WAITING && pcb == socket && packet->tot_len == sizeof(data) &&
                   port == server_port && ip_addr_cmp(address, &remote) && now < deadline &&
                   pbuf_copy_partial(packet, data, sizeof(data), 0) == sizeof(data);
    pbuf_free(packet);
    if (!matches || (data[0] & 7) != 4 || ((data[0] >> 3) & 7) < 3 || ((data[0] >> 3) & 7) > 4 ||
        memcmp(data + 24, nonce, 8))
        return;
    if (!data[1]) {
        /* Respect access refusal and rate limiting, even across link flaps. */
        denied = !memcmp(data + 12, "DENY", 4) || !memcmp(data + 12, "RSTR", 4);
        unsigned poll = data[2] >= 4 && data[2] <= 17 ? 1u << data[2] : 3600;
        if (poll < 60) poll = 60;
        retry();
        earliest_request = time_us() + (uint64_t)poll * SECOND;
        if (denied) system_clock_network_status(CLOCK_NETWORK_REFUSED);
        return;
    }
    uint64_t receive_us, transmit_us;
    if ((data[0] >> 6) == 3 || data[1] > 15 || !timestamp(data + 32, &receive_us) ||
        !timestamp(data + 40, &transmit_us) || transmit_us < receive_us || now < sent_at ||
        now - sent_at > 5 * SECOND || transmit_us - receive_us > now - sent_at + 1000)
        return;
    uint64_t roundtrip = now - sent_at, processing = transmit_us - receive_us;
    uint64_t utc = transmit_us + (roundtrip > processing ? (roundtrip - processing) / 2 : 0);
    if (!system_clock_synchronize(utc)) {
        retry();
        return;
    }
    cancel();
    retry_seconds = 15;
    next_attempt = now + 3600 * SECOND;
    system_clock_network_status(CLOCK_NETWORK_SYNCED);
}

static void resolved(const char *name, const ip_addr_t *address, void *context) {
    (void)name;
    dns_busy = false;
    if ((unsigned)(uintptr_t)context != generation || phase != RESOLVING || !connected) return;
    if (!address || ip_addr_isany(address) || ip_addr_ismulticast(address)) {
        retry();
        return;
    }
    remote = *address;
    socket = udp_new();
    if (!socket || udp_bind(socket, IP_ADDR_ANY, 0) != ERR_OK) {
        retry();
        return;
    }
    udp_recv(socket, received, 0);
    struct pbuf *packet = pbuf_alloc(PBUF_TRANSPORT, 48, PBUF_RAM);
    if (!packet) {
        retry();
        return;
    }
    unsigned char data[48] = {0};
    data[0] = 0x23;
    data[2] = 6;
    data[3] = (unsigned char)-20;
    /* An unpredictable transmit cookie also works before UTC is known. */
    put32(nonce, network_random());
    put32(nonce + 4, network_random());
    if (!get32(nonce) && !get32(nonce + 4)) nonce[7] = 1;
    memcpy(data + 40, nonce, 8);
    pbuf_take(packet, data, sizeof(data));
    sent_at = time_us();
    deadline = sent_at + 5 * SECOND;
    earliest_request = sent_at + 15 * SECOND;
    phase = WAITING;
    err_t error = udp_sendto(socket, packet, &remote, server_port);
    pbuf_free(packet);
    if (error != ERR_OK) retry();
}

void network_time_init(const char *args) {
    strcpy(server, "time.cloudflare.com");
    server_port = 123;
    /* An operator may choose a local time service without exposing settings
     * to web pages. An optional port also permits isolated emulator fixtures. */
    while (args && *args) {
        while (*args == ' ') ++args;
        const char *start = args;
        while (*args && *args != ' ') ++args;
        size_t n = (size_t)(args - start);
        static const char prefix[] = "sos2.ntp=";
        const size_t skip = sizeof(prefix) - 1;
        if (n > skip && n < sizeof(server) && !memcmp(start, prefix, skip)) {
            char value[254];
            memcpy(value, start + skip, n - skip);
            value[n - skip] = 0;
            char *colon = strchr(value, ':');
            unsigned port = 123;
            if (colon) {
                *colon++ = 0;
                port = 0;
                for (; *colon && port <= 65535; ++colon) {
                    if (*colon < '0' || *colon > '9') {
                        port = 0;
                        break;
                    }
                    port = port * 10 + (*colon - '0');
                }
            }
            if (value[0] && port && port <= 65535) {
                strcpy(server, value);
                server_port = (uint16_t)port;
            }
        }
    }
    socket = 0;
    phase = IDLE;
    generation = 0;
    retry_seconds = 15;
    connected = dns_busy = denied = was_ready = false;
    next_attempt = earliest_request = deadline = sent_at = 0;
}

void network_time_poll(bool online) {
    uint64_t now = time_us();
    if (connected != online) {
        connected = online;
        cancel();
        next_attempt = now;
        retry_seconds = 15;
        system_clock_network_status(online ? CLOCK_NETWORK_SYNCING : CLOCK_NETWORK_OFFLINE);
    }
    bool ready = system_clock_view()->ready;
    if (was_ready && !ready) next_attempt = now;
    was_ready = ready;
    if (!connected) return;
    if (denied) {
        system_clock_network_status(CLOCK_NETWORK_REFUSED);
        return;
    }
    if (phase != IDLE && now >= deadline) retry();
    if (phase != IDLE || dns_busy || now < next_attempt || now < earliest_request) return;
    phase = RESOLVING;
    dns_busy = true;
    ++generation;
    deadline = now + 10 * SECOND;
    system_clock_network_status(CLOCK_NETWORK_SYNCING);
    ip_addr_t address;
    err_t error = dns_gethostbyname(server, &address, resolved, (void *)(uintptr_t)generation);
    if (error == ERR_OK) resolved(server, &address, (void *)(uintptr_t)generation);
    else if (error != ERR_INPROGRESS) {
        dns_busy = false;
        retry();
    }
}
