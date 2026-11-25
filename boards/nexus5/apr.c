#include "nexus5.h"
#include "platform.h"
#include "qcom_smd.h"
#include "wire.h"

/* The audio DSP's services through APR, its packet router, on the SMD
 * channel "apr_audio_svc", as the stock kernel speaks it
 * (arch/arm/mach-msm/qdsp6v2/apr.c, apr_tal.c; include/sound/
 * apr_audio-v2.h): a packet is a 20-byte header (its kind, header length
 * and version; its size; the source service, domain and port; the
 * destination's; a token; an opcode) and a payload. Commands go to the
 * DSP's domain (4) from the apps processor's (5); each is answered by a
 * basic result (the command's opcode and a status) or a response of its
 * own, matched here by opcode and token. Events (an ASM buffer played)
 * arrive the same way and go to the caller's handler. Polled on CPU 0. */
enum {
    HEADER = 20,
    SEQUENCED_COMMAND = 0x0250, /* message type 2, header 5 words, version 0 */
    DOMAIN_ADSP = 4,
    DOMAIN_APPS = 5,
    BASIC_RESULT = 0x000110e8,
    PACKET_MAX = 512,
    ANSWER_US = 1000000
};

static struct smd_channel channel;

static enum { CLOSED, OPENING, OPEN, FAILED } state;

static void (*event_handler)(const uint8_t *packet, unsigned size);
/* The answer being waited for: a basic result to this opcode, or a
 * response with its own opcode, carrying this token. */
static uint32_t awaited_opcode, awaited_response, awaited_token;
static bool answered;
static uint32_t answer_status;
static uint8_t answer[64] __attribute__((aligned(4)));

void nexus5_apr_events(void (*handler)(const uint8_t *packet, unsigned size)) {
    event_handler = handler;
}

static bool receive(void) {
    static uint8_t packet[PACKET_MAX] __attribute__((aligned(4)));
    unsigned size = 0;
    if (smd_receive(&channel, packet, sizeof(packet), &size, time_us()) != SMD_OK) return false;
    unsigned header = (packet[0] >> 4 & 15) * 4;
    if (size < HEADER || header < HEADER || header > size || le16(packet + 2) > size) return true;
    uint32_t opcode = le32(packet + 16), token = le32(packet + 12);
    const uint8_t *payload = packet + header;
    unsigned length = size - header;
    if (opcode == BASIC_RESULT && length >= 8 && le32(payload) == awaited_opcode &&
        token == awaited_token && !answered) {
        answer_status = le32(payload + 4);
        answered = true;
    } else if (awaited_response && opcode == awaited_response && token == awaited_token &&
               !answered) {
        memcpy(answer, payload, length < sizeof(answer) ? length : sizeof(answer));
        answer_status = 0;
        answered = true;
    } else if (event_handler) {
        event_handler(packet, size);
    }
    return true;
}

bool nexus5_apr_poll(void) {
    if (state == FAILED) return false;
    if (state == CLOSED) {
        /* The DSP lists its channels a moment after it starts. */
        if (!nexus5_adsp_channel("apr_audio_svc", &channel, time_us())) return false;
        state = OPENING;
    }
    if (state == OPENING) {
        int status = smd_poll(&channel, time_us());
        if (status == SMD_AGAIN) return true;
        if (status != SMD_OK) {
            state = FAILED;
            return false;
        }
        state = OPEN;
    }
    while (receive()) { }
    if (channel.failed) state = FAILED;
    return state == OPEN;
}

/* A packet to service svc at port, with the header filled in. */
bool nexus5_apr_send(uint8_t *packet, unsigned size, uint8_t svc, uint16_t port, uint32_t token,
                     uint32_t opcode) {
    if (size < HEADER || size > PACKET_MAX) return false;
    put16(packet, SEQUENCED_COMMAND);
    put16(packet + 2, (uint16_t)size);
    packet[4] = svc;
    packet[5] = DOMAIN_APPS;
    put16(packet + 6, port);
    packet[8] = svc;
    packet[9] = DOMAIN_ADSP;
    put16(packet + 10, port);
    put32(packet + 12, token);
    put32(packet + 16, opcode);
    for (uint64_t start = time_us();;) {
        if (!nexus5_apr_poll() && state != OPENING) return false;
        if (state == OPEN) {
            int status = smd_send(&channel, packet, size, time_us());
            if (status == SMD_OK) return true;
            if (status != SMD_AGAIN) return false;
        }
        if (time_us() - start >= ANSWER_US) return false;
    }
}

/* A command and its answer: a basic result, success when its status is 0,
 * or the response (copied to out, up to 64 bytes of its payload). */
int nexus5_apr_command(uint8_t *packet, unsigned size, uint8_t svc, uint16_t port, uint32_t token,
                       uint32_t opcode, uint32_t response, void *out, unsigned out_size) {
    awaited_opcode = opcode;
    awaited_response = response;
    awaited_token = token;
    answered = false;
    if (!nexus5_apr_send(packet, size, svc, port, token, opcode)) return -1;
    for (uint64_t start = time_us(); !answered;) {
        if (!nexus5_apr_poll()) return -2;
        if (time_us() - start >= ANSWER_US) return -3;
    }
    awaited_opcode = awaited_response = 0;
    if (out) memcpy(out, answer, out_size < sizeof(answer) ? out_size : sizeof(answer));
    return answer_status ? 1 : 0;
}
