#include "qcom_smd.h"
#include "platform.h"

/* SMD packet ABI: 20-byte length header, two control halves (20 byte flags or
 * 44 word flags), equal FIFOs with a four-byte full/empty gap. Wire reference:
 * vendor smd.c; the combined FIFO size must be a multiple of 32.
 * All peer-controlled indices and lengths are checked before memory access. */
enum {
    CLOSED = 0,
    OPENING = 1,
    OPENED = 2,
    FLUSHING = 3,
    CLOSING = 4,
    RESET_OPENING = 6,
    HEADER = 20,
    RESERVE = 4
};

static bool remote_open(unsigned state) { return state == OPENED || state == FLUSHING; }

static unsigned wrap(const struct smd_channel *c, unsigned at) {
    return at >= c->size ? at - c->size : at;
}

static unsigned used(const struct smd_channel *c, unsigned head, unsigned tail) {
    return head >= tail ? head - tail : c->size - tail + head;
}

static uint32_t read_word(const volatile unsigned char *p) {
    uint32_t v = *(const volatile uint32_t *)p;
    barrier();
    return v;
}

static void write_word(volatile unsigned char *p, uint32_t v) {
    barrier();
    *(volatile uint32_t *)p = v;
}

static unsigned index_offset(const struct smd_channel *c, bool head) {
    return c->half - 8 + (head ? 4 : 0);
}

static unsigned flag_offset(const struct smd_channel *c, unsigned flag) {
    return 4 + flag * (c->words ? 4 : 1);
}

static unsigned flag_get(struct smd_channel *c, bool rx, unsigned flag) {
    volatile unsigned char *p = c->info + (rx ? c->half : 0) + flag_offset(c, flag);
    return c->words ? read_word(p) : *p;
}

static void flag_set(struct smd_channel *c, bool rx, unsigned flag, unsigned value) {
    volatile unsigned char *p = c->info + (rx ? c->half : 0) + flag_offset(c, flag);
    if (c->words) write_word(p, value);
    else *p = (unsigned char)value;
}

static void signal(struct smd_channel *c) {
    barrier();
    c->signal(c->context);
}

static void state(struct smd_channel *c, unsigned value) {
    for (unsigned i = 0; i < 3; ++i) flag_set(c, false, i, value == OPENED);
    write_word(c->info, value);
    flag_set(c, false, 6, 1);
    c->state = value;
    signal(c);
}

void smd_close(struct smd_channel *c) {
    if (!c) return;
    if (c->info && c->signal) state(c, CLOSED);
    c->failed = true;
}

static int fail(struct smd_channel *c) {
    smd_close(c);
    return SMD_FAILED;
}

static bool span(uintptr_t at, size_t n) { return at && n && n <= UINTPTR_MAX - at; }

bool smd_open(struct smd_channel *c, volatile void *info, size_t info_size, volatile void *fifo,
              size_t fifo_size, void (*notify)(void *), void *context, uint64_t now) {
    if (!c) return false;
    *c = (struct smd_channel){.failed = true};
    uintptr_t a = (uintptr_t)info, b = (uintptr_t)fifo;
    size_t half = fifo_size / 2;
    if (!notify || (info_size != 40 && info_size != 88) || !span(a, info_size) ||
        !span(b, fifo_size) || (a & 3) || (b & 3) || !smd_fifo_size_valid(fifo_size) ||
        (a < b + fifo_size && b < a + info_size) || now > UINT64_MAX - 2000000)
        return false;
    /* Never seize a channel with an active local endpoint or invalid remote state. */
    unsigned remote = read_word((volatile unsigned char *)info + info_size / 2);
    unsigned local = read_word(info);
    if ((local != CLOSED && local != CLOSING) || remote > RESET_OPENING) return false;
    *c = (struct smd_channel){.info = info,
                              .tx = fifo,
                              .rx = (volatile unsigned char *)fifo + half,
                              .size = (unsigned)half,
                              .half = (unsigned)(info_size / 2),
                              .words = info_size == 88,
                              .deadline = now + 2000000,
                              .signal = notify,
                              .context = context,
                              .peer_opening = !remote_open(remote)};
    unsigned head = index_offset(c, true), tail = index_offset(c, false);
    /* LK can close a drained channel without clearing its indices. Reset only
     * our producer/consumer; the peer resets its own during the open handshake. */
    const unsigned offsets[] = {head, tail, c->half + head, c->half + tail};
    for (unsigned i = 0; i < 4; ++i) {
        unsigned index = read_word(c->info + offsets[i]);
        if (index >= c->size || (c->words && (index & 3))) {
            fail(c);
            return false;
        }
    }
    write_word(c->info + head, 0);
    write_word(c->info + c->half + tail, 0);
    for (unsigned i = 3; i <= 5; ++i) flag_set(c, false, i, 0);
    flag_set(c, false, 7, 0); /* Request notifications when the remote frees space. */
    state(c, OPENING);
    return true;
}

bool smd_adopt_idle(struct smd_channel *c, volatile void *info, size_t info_size,
                    volatile void *fifo, size_t fifo_size, void (*notify)(void *), void *context) {
    if (!c) return false;
    *c = (struct smd_channel){.failed = true};
    uintptr_t a = (uintptr_t)info, b = (uintptr_t)fifo;
    size_t half = fifo_size / 2;
    if (!notify || (info_size != 40 && info_size != 88) || !span(a, info_size) ||
        !span(b, fifo_size) || (a & 3) || (b & 3) || !smd_fifo_size_valid(fifo_size) ||
        (a < b + fifo_size && b < a + info_size))
        return false;
    struct smd_channel candidate = {.info = info,
                                    .tx = fifo,
                                    .rx = (volatile unsigned char *)fifo + half,
                                    .size = half,
                                    .half = info_size / 2,
                                    .words = info_size == 88,
                                    .signal = notify,
                                    .context = context,
                                    .state = OPENED};
    unsigned head = index_offset(&candidate, true), tail = index_offset(&candidate, false);
    candidate.head = read_word(candidate.info + head);
    candidate.tail = read_word(candidate.info + candidate.half + tail);
    if (read_word(candidate.info) != OPENED ||
        read_word(candidate.info + candidate.half) != OPENED || candidate.head >= half ||
        candidate.tail >= half || (candidate.words && ((candidate.head | candidate.tail) & 3)) ||
        read_word(candidate.info + tail) != candidate.head ||
        read_word(candidate.info + candidate.half + head) != candidate.tail)
        return false;
    barrier();
    if (read_word(candidate.info + head) != candidate.head ||
        read_word(candidate.info + candidate.half + tail) != candidate.tail ||
        read_word(candidate.info + tail) != candidate.head ||
        read_word(candidate.info + candidate.half + head) != candidate.tail)
        return false;
    *c = candidate;
    return true;
}

int smd_poll(struct smd_channel *c, uint64_t now) {
    if (!c || c->failed) return SMD_FAILED;
    unsigned head = index_offset(c, true), tail = index_offset(c, false);
    unsigned remote = read_word(c->info + c->half);
    if (read_word(c->info) != c->state || remote > RESET_OPENING ||
        read_word(c->info + head) != c->head || read_word(c->info + c->half + tail) != c->tail ||
        read_word(c->info + tail) >= c->size || read_word(c->info + c->half + head) >= c->size)
        return fail(c);
    if (c->words && ((read_word(c->info + tail) | read_word(c->info + c->half + head)) & 3))
        return fail(c);
    flag_set(c, true, 6, 0);
    /* A new peer can open and publish its first packet between polls. Stock
     * SMD accepts that OPENED transition without requiring an empty RX FIFO.
     * An inherited OPENED state alone still cannot establish a fresh session. */
    if (c->state == OPENING && !remote_open(remote)) c->peer_opening = true;
    /* OPENING alone is not an acknowledgement. Stock stays OPENING until
     * the peer publishes OPENED; a closing/reset handoff can settle meanwhile. */
    if (c->state == OPENING && remote == OPENED && !read_word(c->info + tail) &&
        (!read_word(c->info + c->half + head) || c->peer_opening))
        state(c, OPENED);
    if (c->state == OPENED) {
        if (!remote_open(remote)) return fail(c);
        c->deadline = 0;
        return SMD_OK;
    }
    if (!c->deadline || now >= c->deadline) return fail(c);
    return SMD_AGAIN;
}

static void copy_out(struct smd_channel *c, unsigned at, const unsigned char *data, unsigned n) {
    for (unsigned i = 0; i < n;) {
        if (c->words) {
            uint32_t v = (uint32_t)data[i] | (uint32_t)data[i + 1] << 8 |
                         (uint32_t)data[i + 2] << 16 | (uint32_t)data[i + 3] << 24;
            *(volatile uint32_t *)(c->tx + wrap(c, at + i)) = v;
            i += 4;
        } else {
            c->tx[wrap(c, at + i)] = data[i];
            ++i;
        }
    }
}

static void copy_in(struct smd_channel *c, unsigned at, unsigned char *data, unsigned n) {
    for (unsigned i = 0; i < n;) {
        if (c->words) {
            uint32_t v = *(const volatile uint32_t *)(c->rx + wrap(c, at + i));
            for (unsigned j = 0; j < 4; ++j) data[i + j] = (unsigned char)(v >> (j * 8));
            i += 4;
        } else {
            data[i] = c->rx[wrap(c, at + i)];
            ++i;
        }
    }
}

int smd_send(struct smd_channel *c, const void *data, unsigned n, uint64_t now) {
    if (!c || !data || !n || n > SMD_PACKET_MAX || n + HEADER + RESERVE > c->size ||
        (c->words && n % 4))
        return SMD_INVALID;
    int status = smd_poll(c, now);
    if (status != SMD_OK) return status;
    unsigned tail = read_word(c->info + index_offset(c, false));
    /* Snapshot again before using a value the remote may change asynchronously. */
    if (tail >= c->size || (c->words && tail % 4)) return fail(c);
    unsigned free = c->size - used(c, c->head, tail),
             available = free < RESERVE ? 0 : free - RESERVE;
    if (available < n + HEADER) return SMD_AGAIN;
    unsigned char header[HEADER] = {0};
    for (unsigned i = 0; i < 4; ++i) header[i] = (unsigned char)(n >> (i * 8));
    copy_out(c, c->head, header, HEADER);
    copy_out(c, c->head + HEADER, data, n);
    if (!remote_open(read_word(c->info + c->half))) return fail(c);
    flag_set(c, false, 5, 0);
    c->head = wrap(c, c->head + HEADER + n);
    write_word(c->info + index_offset(c, true), c->head);
    flag_set(c, false, 4, 1);
    signal(c);
    return SMD_OK;
}

int smd_receive(struct smd_channel *c, void *data, unsigned capacity, unsigned *size,
                uint64_t now) {
    if (size) *size = 0;
    if (!c || !data || !size) return SMD_INVALID;
    int status = smd_poll(c, now);
    if (status != SMD_OK) return status;
    unsigned head = read_word(c->info + c->half + index_offset(c, true));
    if (head >= c->size || (c->words && head % 4)) return fail(c);
    unsigned available = used(c, head, c->tail);
    if (available < HEADER) return SMD_AGAIN;
    unsigned char header[4];
    copy_in(c, c->tail, header, 4);
    uint32_t n = (uint32_t)header[0] | (uint32_t)header[1] << 8 | (uint32_t)header[2] << 16 |
                 (uint32_t)header[3] << 24;
    if (!n || n > SMD_PACKET_MAX || n + HEADER + RESERVE > c->size || (c->words && n % 4))
        return fail(c);
    if (n > capacity) return SMD_INVALID;
    if (available < n + HEADER) return SMD_AGAIN;
    copy_in(c, c->tail + HEADER, data, n);
    if (!remote_open(read_word(c->info + c->half))) return fail(c);
    c->tail = wrap(c, c->tail + HEADER + n);
    flag_set(c, true, 4, 0);
    write_word(c->info + c->half + index_offset(c, false), c->tail);
    flag_set(c, true, 5, 1);
    barrier();
    if (!flag_get(c, true, 7)) signal(c);
    *size = n;
    return SMD_OK;
}
