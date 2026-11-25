#include "nexus5.h"
#include "platform.h"
#include "qcom_smd.h"
#include "wire.h"

/* The audio DSP's QMI services over its IPC router link, as the stock
 * kernel reaches them (arch/arm/mach-msm/ipc_router.c, its SMD transport
 * ipc_router_smd_xprt.c and msm_qmi_interface.c), here for the one service
 * SOS2 needs, the DSP's SLIMbus manager (slim-msm.c's QMI client). The link
 * is the DSP's SMD channel "IPCRTR", a packet each: a router header (version,
 * type, source node and port, a request to confirm receipt, the payload's
 * size, destination node and port) and a payload. The DSP says HELLO, this
 * side answers HELLO, and the DSP lists its services (NEW_SERVER: service,
 * instance, node, port). A QMI request is a DATA packet from our client
 * port to the service's address, its 7-byte header (request, transaction,
 * message, length) and TLVs; its response comes back to our port, a TLV 2
 * with the result. Everything is polled on CPU 0. */
enum {
    ROUTER_VERSION = 1,
    DATA = 1,
    HELLO = 2,
    NEW_SERVER = 4,
    REMOVE_SERVER = 5,
    RESUME_TX = 7,
    HEADER = 32,
    LOCAL_NODE = 1,
    CONTROL_PORT = 0xfffffffe,
    CLIENT_PORT = 0x4000,
    PACKET_MAX = 512,
    SERVERS = 16,
    QMI_HEADER = 7,
    QMI_REQUEST = 0,
    QMI_RESPONSE = 2,
    LINK_US = 1000000
};

struct server {
    uint32_t service, instance, node, port;
};

static struct smd_channel link;

static enum { CLOSED, OPENING, OPEN, FAILED } state;

static bool hello;
static uint32_t remote_node;
static struct server servers[SERVERS];
static unsigned server_count;
static uint16_t transaction;
/* The response being waited for: its transaction, then its bytes. */
static uint16_t awaited;
static uint8_t response[PACKET_MAX] __attribute__((aligned(4)));
static unsigned response_length;
static bool responded;

static bool send(uint32_t type, uint32_t source_port, uint32_t node, uint32_t port,
                 const uint8_t *payload, unsigned size) {
    static uint8_t packet[PACKET_MAX] __attribute__((aligned(4)));
    if (HEADER + size > sizeof(packet)) return false;
    const uint32_t header[] = {ROUTER_VERSION, type, LOCAL_NODE, source_port, 0, size, node, port};
    for (unsigned i = 0; i < 8; ++i) put32(packet + 4 * i, header[i]);
    memcpy(packet + HEADER, payload, size);
    /* Room comes back as the DSP reads; a full FIFO waits a little. */
    for (uint64_t start = time_us();;) {
        int status = smd_send(&link, packet, HEADER + size, time_us());
        if (status == SMD_OK) return true;
        if (status != SMD_AGAIN || time_us() - start >= LINK_US) return false;
    }
}

static bool send_control(const uint32_t *words, unsigned count) {
    uint8_t body[20] = {0};
    for (unsigned i = 0; i < count && i < 5; ++i) put32(body + 4 * i, words[i]);
    return send(words[0], CONTROL_PORT, remote_node, CONTROL_PORT, body, sizeof(body));
}

static void control(const uint8_t *body, unsigned size, uint32_t source_node) {
    uint32_t command = size >= 4 ? le32(body) : 0;
    if (command == HELLO) {
        remote_node = source_node;
        if (!hello) {
            const uint32_t reply[] = {HELLO};
            hello = send_control(reply, 1);
        }
    } else if (command == NEW_SERVER && size >= 20 && server_count < SERVERS) {
        struct server s = {le32(body + 4), le32(body + 8), le32(body + 12), le32(body + 16)};
        for (unsigned i = 0; i < server_count; ++i)
            if (servers[i].service == s.service && servers[i].instance == s.instance) {
                servers[i] = s;
                return;
            }
        servers[server_count++] = s;
    } else if (command == REMOVE_SERVER && size >= 20) {
        for (unsigned i = 0; i < server_count; ++i)
            if (servers[i].node == le32(body + 12) && servers[i].port == le32(body + 16))
                servers[i] = servers[--server_count];
    }
}

/* One packet from the DSP, if one waits: false when none does. */
static bool receive(void) {
    static uint8_t packet[PACKET_MAX] __attribute__((aligned(4)));
    unsigned size = 0;
    if (smd_receive(&link, packet, sizeof(packet), &size, time_us()) != SMD_OK) return false;
    if (size < HEADER || le32(packet) != ROUTER_VERSION || le32(packet + 20) > size - HEADER)
        return true;
    uint32_t type = le32(packet + 4), source_node = le32(packet + 8), confirm = le32(packet + 16),
             length = le32(packet + 20), port = le32(packet + 28);
    const uint8_t *body = packet + HEADER;
    if (port == CONTROL_PORT || type == HELLO) control(body, length, source_node);
    else if (type == DATA && port == CLIENT_PORT && length >= QMI_HEADER &&
             body[0] == QMI_RESPONSE && le16(body + 1) == awaited && length <= sizeof(response)) {
        memcpy(response, body, length);
        response_length = length;
        responded = true;
    }
    if (type == DATA && confirm) {
        const uint32_t resume[] = {RESUME_TX, le32(packet + 24), port};
        (void)send_control(resume, 3);
    }
    return true;
}

/* The link open, every packet waiting read. */
static bool poll(void) {
    if (state == FAILED) return false;
    if (state == CLOSED) {
        /* The DSP lists its channels a moment after it starts. */
        if (!nexus5_adsp_channel("IPCRTR", &link, time_us())) return false;
        state = OPENING;
    }
    if (state == OPENING) {
        int status = smd_poll(&link, time_us());
        if (status == SMD_AGAIN) return true;
        if (status != SMD_OK) {
            state = FAILED;
            return false;
        }
        state = OPEN;
    }
    while (receive()) { }
    if (link.failed) state = FAILED;
    return state == OPEN;
}

static const struct server *find(uint32_t service, uint32_t instance) {
    for (unsigned i = 0; i < server_count; ++i)
        if (servers[i].service == service && servers[i].instance == instance) return &servers[i];
    return 0;
}

int nexus5_qmi_request(uint32_t service, uint32_t instance, uint16_t message, const uint8_t *tlvs,
                       unsigned length, uint64_t wait_us) {
    uint64_t start = time_us();
    const struct server *s = 0;
    while (!(s = find(service, instance))) {
        if (!poll() && state == FAILED) return -1;
        if (time_us() - start >= wait_us) return -2;
    }
    uint8_t request[PACKET_MAX - HEADER];
    if (QMI_HEADER + length > sizeof(request)) return -3;
    awaited = ++transaction;
    responded = false;
    request[0] = QMI_REQUEST;
    put16(request + 1, awaited);
    put16(request + 3, message);
    put16(request + 5, (uint16_t)length);
    memcpy(request + QMI_HEADER, tlvs, length);
    if (!send(DATA, CLIENT_PORT, s->node, s->port, request, QMI_HEADER + length)) return -4;
    while (!responded) {
        if (!poll()) return -5;
        if (time_us() - start >= wait_us) return -6;
    }
    /* The result TLV: type 2, length 4, the result then the error. */
    for (unsigned at = QMI_HEADER; at + 3 <= response_length;) {
        unsigned type = response[at], size = le16(response + at + 1);
        if (at + 3 + size > response_length) break;
        if (type == 2 && size == 4)
            return le16(response + at + 3) ? (int)(0x10000 | le16(response + at + 5)) : 0;
        at += 3 + size;
    }
    return -7;
}

void nexus5_qmi_poll(void) { (void)poll(); }

bool nexus5_qmi_has(uint32_t service, uint32_t instance) {
    (void)poll();
    return find(service, instance) != 0;
}
