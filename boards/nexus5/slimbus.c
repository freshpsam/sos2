#include "nexus5.h"
#include "platform.h"

/* The apps processor's place on MSM8974's SLIMbus, whose manager is the
 * audio DSP, as the stock kernel takes it (drivers/slimbus/slim-msm-ngd.c,
 * slim-msm.c and slimbus.c): a satellite through the controller's
 * non-ported generic device (NGD), its messages through registers (no
 * message queues). The DSP powers the bus for it on request (its SLIMbus
 * QMI service: select instance 0 with the DSP as master, then power
 * active); once the NGD is enabled the DSP announces its capability, which
 * the satellite answers (REPORT_SATELLITE), and from then on it asks the
 * DSP for devices' logical addresses (ADDR_QUERY), reads and changes their
 * value elements itself (REQUEST_VALUE / CHANGE_VALUE: the codec's
 * registers), and asks the DSP to connect ports and define channels. A
 * message is a header (its length, type, code, then the destination's
 * logical address) and a payload, at most 40 bytes, a word at a time. */
enum {
    SLIM = 0xfe12f000,
    NGD_V1 = 0x800, /* this controller's NGD (cell index 1) by layout */
    NGD_V2 = 0x1000,
    CFG = 0x00,
    STATUS = 0x04,
    INT_EN = 0x10,
    INT_STAT = 0x14,
    INT_CLR = 0x18,
    TX_MSG = 0x30,
    RX_MSG = 0x70,
    ENABLE = 1,
    LADDR = 1 << 1,
    TX_NACKED_2 = 1 << 25,
    MSG_BUF_CONTE = 1 << 26,
    MSG_TX_INVAL = 1 << 27,
    IE_VE_CHG = 1 << 28,
    DEV_ERR = 1 << 29,
    RX_MSG_RCVD = 1 << 30,
    TX_MSG_SENT = 1u << 31,
    TX_ERRORS = TX_NACKED_2 | MSG_BUF_CONTE | MSG_TX_INVAL | DEV_ERR,
    MT_CORE = 0,
    MT_DEST_USER = 2,
    MT_SRC_USER = 6,
    MC_MASTER_CAPABILITY = 0x00,
    MC_REPORT_SATELLITE = 0x01,
    MC_ADDR_QUERY = 0x0d,
    MC_ADDR_REPLY = 0x0e,
    MC_GENERIC_ACK = 0x25,
    MC_DEF_ACT_CHAN = 0x21,
    MC_CHAN_CTRL = 0x23,
    MC_RECONFIG_NOW = 0x24,
    MC_CONNECT_SINK = 0x2d,
    CHANNEL_REMOVE = 2, /* slimbus.h's SLIM_CH_REMOVE */
    MC_REQUEST_VALUE = 0x60,
    MC_REPLY_VALUE = 0x64,
    MC_CHANGE_VALUE = 0x68,
    LA_MANAGER = 0xff,
    QMI_SERVICE = 0x301,
    QMI_INSTANCE = 1,
    QMI_SELECT_INSTANCE = 0x20,
    QMI_POWER = 0x21,
    QMI_MASTER = 2,
    QMI_ACTIVE = 2,
    QMI_INACTIVE = 1,
    QMI_US = 2000000,
    MESSAGE_US = 100000,
    SAT_SUCCESS = 0x20
};

static uintptr_t ngd;
static bool selected;
static uint8_t next_tid;

static uint32_t reg(unsigned offset) { return mmio_read32(ngd + offset); }

static void set(unsigned offset, uint32_t value) { nexus5_store(ngd + offset, value); }

/* The DSP's SLIMbus service: the instance once (TLV 1: instance 0; TLV
 * 0x10: the DSP as master), then power on or off (TLV 1: the mode). */
static bool power(bool active) {
    if (!selected) {
        static const uint8_t select[] = {0x01, 4, 0, 0, 0, 0, 0, 0x10, 4, 0, QMI_MASTER, 0, 0, 0};
        if (nexus5_qmi_request(QMI_SERVICE, QMI_INSTANCE, QMI_SELECT_INSTANCE, select,
                               sizeof(select), QMI_US))
            return false;
        selected = true;
    }
    const uint8_t request[] = {0x01, 4, 0, active ? QMI_ACTIVE : QMI_INACTIVE, 0, 0, 0};
    return !nexus5_qmi_request(QMI_SERVICE, QMI_INSTANCE, QMI_POWER, request, sizeof(request),
                               QMI_US);
}

/* A message out: its words written from the first, then sent (or refused). */
static bool send(const uint8_t *message, unsigned length) {
    uint32_t words[10] = {0};
    if (!length || length > sizeof(words)) return false;
    memcpy(words, message, length);
    set(INT_CLR, TX_MSG_SENT | TX_ERRORS);
    for (unsigned i = 0; i < (length + 3) / 4; ++i) set(TX_MSG + 4 * i, words[i]);
    __asm__ volatile("dsb" ::: "memory");
    for (uint64_t start = time_us(); time_us() - start < MESSAGE_US;) {
        uint32_t status = reg(INT_STAT);
        if (status & TX_ERRORS) {
            set(INT_CLR, status & TX_ERRORS);
            return false;
        }
        if (status & TX_MSG_SENT) {
            set(INT_CLR, TX_MSG_SENT);
            return true;
        }
    }
    return false;
}

/* A message in, if one arrived: its length, else 0. */
static unsigned receive(uint8_t *out) {
    if (!(reg(INT_STAT) & RX_MSG_RCVD)) return 0;
    uint32_t words[10] = {0};
    words[0] = reg(RX_MSG);
    unsigned length = words[0] & 0x1f;
    for (unsigned i = 1; i < (length + 3) / 4 && i < 10; ++i) words[i] = reg(RX_MSG + 4 * i);
    set(INT_CLR, RX_MSG_RCVD);
    __asm__ volatile("dsb" ::: "memory");
    memcpy(out, words, 40);
    return length;
}

/* A message's three header bytes to a logical address. */
static unsigned header(uint8_t *m, unsigned length, unsigned type, unsigned code, uint8_t la) {
    m[0] = (uint8_t)(length | type << 5);
    m[1] = (uint8_t)code;
    m[2] = la;
    return 3;
}

/* Waits for a message of this type and code (and transaction, unless tid
 * is negative), dropping any other. */
static unsigned await(uint8_t *in, unsigned type, unsigned code, int tid) {
    for (uint64_t start = time_us(); time_us() - start < MESSAGE_US * 5;) {
        unsigned length = receive(in);
        if (!length) continue;
        if ((in[0] >> 5) == type && in[1] == code && (tid < 0 || in[3] == tid)) return length;
    }
    return 0;
}

const char *nexus5_slimbus_start(void) {
    if (!power(true)) return "the DSP did not power SLIMbus";
    uint32_t version = mmio_read32(SLIM) >> 16;
    ngd = SLIM + (version ? NGD_V2 : NGD_V1);
    if (reg(STATUS) & LADDR) return 0; /* still enumerated from before */
    set(INT_EN, TX_NACKED_2 | MSG_BUF_CONTE | MSG_TX_INVAL | IE_VE_CHG | DEV_ERR | TX_MSG_SENT |
                    RX_MSG_RCVD);
    set(CFG, ENABLE);
    uint8_t in[40];
    if (!await(in, MT_SRC_USER, MC_MASTER_CAPABILITY, -1))
        return "the DSP did not announce SLIMbus";
    uint8_t m[8];
    unsigned n = header(m, 7, MT_SRC_USER, MC_REPORT_SATELLITE, LA_MANAGER);
    m[n++] = 0xd9; /* the satellite protocol's magic, version and protocol */
    m[n++] = 0xc5;
    m[n++] = 1;
    m[n++] = 1;
    for (unsigned i = 0; i < 10; ++i) {
        if (send(m, n)) return 0;
        delay_us(10000);
    }
    return "the DSP did not take this processor's SLIMbus report";
}

bool nexus5_slimbus_address(const uint8_t enumeration[6], uint8_t *la) {
    uint8_t m[12], in[40];
    uint8_t tid = next_tid++;
    unsigned n = header(m, 10, MT_DEST_USER, MC_ADDR_QUERY, LA_MANAGER);
    m[n++] = tid;
    for (unsigned i = 0; i < 6; ++i) m[n++] = enumeration[i];
    if (!send(m, n) || !await(in, MT_SRC_USER, MC_ADDR_REPLY, tid)) return false;
    static const uint8_t none[6] = {0};
    if (!memcmp(in + 4, none, 6) || in[10] == 0xff) return false;
    *la = in[10];
    return true;
}

/* A byte value element: its element code, the offset with byte access and a
 * one-byte slice. */
static uint16_t element(uint16_t offset) { return (uint16_t)(1 << 3 | (offset & 0xfff) << 4); }

bool nexus5_slimbus_write(uint8_t la, uint16_t offset, uint8_t value) {
    uint8_t m[8];
    uint16_t code = element(offset);
    unsigned n = header(m, 6, MT_CORE, MC_CHANGE_VALUE, la);
    m[n++] = (uint8_t)code;
    m[n++] = (uint8_t)(code >> 8);
    m[n++] = value;
    return send(m, n);
}

bool nexus5_slimbus_read(uint8_t la, uint16_t offset, uint8_t *value) {
    uint8_t m[8], in[40];
    uint8_t tid = next_tid++;
    uint16_t code = element(offset);
    unsigned n = header(m, 6, MT_CORE, MC_REQUEST_VALUE, la);
    m[n++] = tid;
    m[n++] = (uint8_t)code;
    m[n++] = (uint8_t)(code >> 8);
    if (!send(m, n) || !await(in, MT_CORE, MC_REPLY_VALUE, tid)) return false;
    *value = in[4];
    return true;
}

/* A user message to the manager and its generic acknowledgement: payload
 * bytes with the transaction ID at tid_at. */
static bool manager(unsigned code, uint8_t *payload, unsigned length, unsigned tid_at) {
    uint8_t m[40], in[40];
    uint8_t tid = next_tid++;
    payload[tid_at] = tid;
    unsigned n = header(m, 3 + length, MT_DEST_USER, code, LA_MANAGER);
    memcpy(m + n, payload, length);
    if (!send(m, n + length) || !await(in, MT_SRC_USER, MC_GENERIC_ACK, tid)) return false;
    return in[4] & SAT_SUCCESS;
}

/* slim-msm-ngd.c's connect (a device's port, as sink, to a channel), its
 * define-and-activate (the channel's properties: data format, segment
 * length, coefficient, auxiliary bits, root exponent, protocol, presence
 * rate, then the channels) and its reconfiguration. */
bool nexus5_slimbus_connect_sink(uint8_t la, uint8_t port, uint8_t channel) {
    uint8_t p[4] = {la, port, channel, 0};
    return manager(MC_CONNECT_SINK, p, sizeof(p), 3);
}

bool nexus5_slimbus_define(uint8_t la, const uint8_t properties[4], uint8_t channel) {
    uint8_t p[6] = {(uint8_t)(properties[0] | (la & 0x1f)),
                    properties[1],
                    properties[2],
                    properties[3],
                    0,
                    channel};
    uint8_t now[2] = {0, la};
    return manager(MC_DEF_ACT_CHAN, p, sizeof(p), 4) && manager(MC_RECONFIG_NOW, now, 2, 0);
}

bool nexus5_slimbus_remove(uint8_t la, uint8_t channel) {
    uint8_t p[3] = {(uint8_t)(CHANNEL_REMOVE << 6 | (la & 0x1f)), 0, channel};
    uint8_t now[2] = {0, la};
    return manager(MC_CHAN_CTRL, p, sizeof(p), 1) && manager(MC_RECONFIG_NOW, now, 2, 0);
}

bool nexus5_slimbus_stop(void) { return power(false); }
