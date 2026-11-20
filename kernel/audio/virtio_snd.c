#include "audio.h"
#include "platform.h"

/* Virtio 1.2 section 5.14, modern MMIO and split queues, polled: QEMU's
 * virtio-sound. One output stream, S16 stereo at 48 kHz, gets the mixer's
 * frames in periods of up to 10 ms, at most four queued at a time; control
 * requests go one at a time. The event and capture queues stay unconfigured:
 * nothing here records or uses the device's events (QEMU sends none). Queues
 * and buffers are static, so uncached, like the other virtio drivers'.
 * https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html */
enum {
    QUEUE_SIZE = 8,
    CONTROL = 0, /* virtqueue indexes */
    TX = 2,
    PERIOD_FRAMES = USER_AUDIO_RATE / 100,
    PERIOD_BYTES = PERIOD_FRAMES * sizeof(struct audio_frame),
    PERIODS = QUEUE_SIZE / 2, /* two descriptors each */
    /* A short period waits while the device holds this much: a writer that
     * keeps up in small pieces then hears no gaps. */
    LOW_WATER = 2 * PERIOD_FRAMES,
    /* The device takes periods into a buffer of its own before playing them,
     * and stopping it would cut that short: it stops only once nothing was
     * queued for this long. */
    STOP_DELAY_US = 250000,
    TIMEOUT_US = 1000000,
    STREAM_LIMIT = 16, /* streams asked about */
    R_PCM_INFO = 0x100,
    R_PCM_SET_PARAMS = 0x101,
    R_PCM_PREPARE = 0x102,
    R_PCM_START = 0x104,
    R_PCM_STOP = 0x105,
    S_OK = 0x8000,
    OUTPUT = 0,
    FORMAT_S16 = 5,
    RATE_48000 = 7
};

_Static_assert(USER_AUDIO_RATE == 48000 && USER_AUDIO_CHANNELS == 2, "the stream's format");

struct descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags, next;
};

struct queue {
    struct descriptor desc[QUEUE_SIZE] __attribute__((aligned(16)));

    volatile struct {
        uint16_t flags, index, ring[QUEUE_SIZE];
    } avail __attribute__((aligned(16)));

    volatile struct {
        uint16_t flags, index;

        struct {
            uint32_t id, length;
        } ring[QUEUE_SIZE];
    } used __attribute__((aligned(16)));

    uint16_t seen; /* used entries taken back */
};

/* struct virtio_snd_pcm_hdr, virtio_snd_query_info and virtio_snd_pcm_set_params. */
struct pcm_request {
    uint32_t code, stream;
};

struct info_request {
    uint32_t code, start, count, size;
};

struct params_request {
    uint32_t code, stream, buffer_bytes, period_bytes, features;
    uint8_t channels, format, rate, padding;
};

static struct queue control, tx;

static union {
    struct pcm_request pcm;
    struct info_request info;
    struct params_request params;
} request __attribute__((aligned(16)));

/* The status, then PCM_INFO's struct virtio_snd_pcm_info, whose 64-bit masks
 * are not 8-byte aligned here. */
static volatile struct {
    uint32_t code, hda_fn_nid, features, formats[2], rates[2];
    uint8_t direction, channels_min, channels_max, padding[5];
} reply __attribute__((aligned(16)));

/* A period: struct virtio_snd_pcm_xfer and the frames, which the device
 * reads, then struct virtio_snd_pcm_status, which it writes. */
static struct {
    uint32_t stream;
    struct audio_frame frames[PERIOD_FRAMES];
} periods[PERIODS] __attribute__((aligned(16)));

static volatile struct {
    uint32_t status, latency;
} results[PERIODS];

static uint32_t lengths[PERIODS]; /* a queued period's frames; 0: free */
static uintptr_t device;
static uint32_t stream, held; /* held: frames queued in the device */
static bool asking;           /* a control request awaits its answer */
static unsigned answer_bytes;
static uint64_t asked_at, filled_at;

static enum { STOPPED, STARTING, RUNNING, STOPPING } state;

static void reg(unsigned offset, uint32_t value) {
    barrier();
    mmio_write32(device + offset, value);
    barrier();
}

static void address(unsigned offset, const volatile void *p) {
    uint64_t value = (uintptr_t)p;
    reg(offset, (uint32_t)value);
    reg(offset + 4, (uint32_t)(value >> 32));
}

/* A reset takes the queues back from the device. */
static bool fail(void) {
    if (device) reg(0x70, 0);
    device = 0;
    held = 0;
    return false;
}

static bool queue_init(unsigned index, struct queue *q) {
    reg(0x30, index);
    if (mmio_read32(device + 0x44) || mmio_read32(device + 0x34) < QUEUE_SIZE) return false;
    memset(q, 0, sizeof(*q));
    q->avail.flags = 1;
    reg(0x38, QUEUE_SIZE);
    address(0x80, q->desc);
    address(0x90, &q->avail);
    address(0xa0, &q->used);
    reg(0x44, 1);
    return true;
}

static void ask(unsigned bytes, unsigned answer) {
    reply.code = 0;
    control.desc[0] = (struct descriptor){(uintptr_t)&request, bytes, 1, 1};
    control.desc[1] = (struct descriptor){(uintptr_t)&reply, answer, 2, 0};
    control.avail.ring[control.avail.index % QUEUE_SIZE] = 0;
    barrier();
    ++control.avail.index;
    barrier();
    reg(0x50, CONTROL);
    answer_bytes = answer;
    asked_at = time_us();
    asking = true;
}

/* 1: the request succeeded, 0: no answer yet, -1: failed, and the device
 * is stopped. */
static int answered(void) {
    uint16_t end = control.used.index;
    barrier();
    if (end == control.seen) {
        if (time_us() - asked_at < TIMEOUT_US) return 0;
        fail();
        return -1;
    }
    unsigned at = control.seen % QUEUE_SIZE;
    if ((uint16_t)(end - control.seen) != 1 || control.used.ring[at].id ||
        control.used.ring[at].length != answer_bytes || reply.code != S_OK) {
        fail();
        return -1;
    }
    ++control.seen;
    asking = false;
    return 1;
}

/* At start-up only: waits for the answer. */
static bool ask_now(unsigned bytes, unsigned answer) {
    ask(bytes, answer);
    int result;
    while (!(result = answered())) { }
    return result > 0;
}

static uint32_t latency(void) { return held; }

/* Takes back the periods the device has finished with. */
static bool reclaim(void) {
    uint16_t end = tx.used.index;
    barrier();
    if ((uint16_t)(end - tx.seen) > PERIODS) return false;
    for (; tx.seen != end; ++tx.seen) {
        unsigned at = tx.seen % QUEUE_SIZE, id = tx.used.ring[at].id, slot = id / 2;
        if ((id & 1) || slot >= PERIODS || !lengths[slot] ||
            tx.used.ring[at].length != sizeof(results[0]) || results[slot].status != S_OK)
            return false;
        held -= lengths[slot];
        lengths[slot] = 0;
    }
    return true;
}

/* Queues the mixer's frames in the free periods: whole periods, or what
 * there is once the device runs low. */
static bool fill(void) {
    bool queued = false;
    for (unsigned slot = 0; slot < PERIODS; ++slot) {
        if (lengths[slot]) continue;
        uint32_t n = audio_ready();
        if (!n || (n < PERIOD_FRAMES && held >= LOW_WATER)) break;
        if (n > PERIOD_FRAMES) n = PERIOD_FRAMES;
        audio_mix(periods[slot].frames, n);
        tx.desc[2 * slot] = (struct descriptor){
            (uintptr_t)&periods[slot],
            (uint32_t)(sizeof(periods[slot].stream) + n * sizeof(struct audio_frame)), 1,
            (uint16_t)(2 * slot + 1)};
        tx.desc[2 * slot + 1] =
            (struct descriptor){(uintptr_t)&results[slot], sizeof(results[slot]), 2, 0};
        tx.avail.ring[tx.avail.index % QUEUE_SIZE] = (uint16_t)(2 * slot);
        barrier();
        ++tx.avail.index;
        lengths[slot] = n;
        held += n;
        queued = true;
    }
    if (queued) {
        barrier();
        reg(0x50, TX);
    }
    return queued;
}

static void pcm(uint32_t code) {
    request.pcm = (struct pcm_request){code, stream};
    ask(sizeof(request.pcm), sizeof(reply.code));
}

static bool poll(void) {
    if (!device) return false;
    /* Idle: nothing queued, nothing to queue, and no device access. */
    if (state == STOPPED && !audio_ready()) return true;
    uint16_t done = tx.seen;
    if (mmio_read32(device + 0x70) != 15 || !reclaim()) return fail();
    if (asking) {
        int result = answered();
        if (result <= 0) return !result;
        state = state == STARTING ? RUNNING : STOPPED;
    }
    uint64_t now = time_us();
    if (fill()) filled_at = now;
    if (state == STOPPED && held) {
        pcm(R_PCM_START);
        state = STARTING;
    } else if (state == RUNNING && !held && now - filled_at >= STOP_DELAY_US) {
        pcm(R_PCM_STOP);
        state = STOPPING;
    }
    if (done != tx.seen) reg(0x64, mmio_read32(device + 0x60) & 3);
    return true;
}

bool virtio_snd_init(const struct platform_devices *devices, struct audio_device *out) {
    device = 0;
    for (size_t i = 0; i < devices->virtio_count && !device; ++i) {
        uintptr_t at = devices->virtio[i];
        if (mmio_read32(at) == 0x74726976 && mmio_read32(at + 4) == 2 && mmio_read32(at + 8) == 25)
            device = at;
    }
    if (!device) return false;
    reg(0x70, 0);
    if (mmio_read32(device + 0x70)) return fail();
    reg(0x70, 3);
    reg(0x14, 1);
    if (!(mmio_read32(device + 0x10) & 1)) return fail();
    reg(0x24, 0);
    reg(0x20, 0);
    reg(0x24, 1);
    reg(0x20, 1);
    reg(0x70, 11);
    if (!(mmio_read32(device + 0x70) & 8)) return fail();
    uint32_t streams = 0;
    bool stable = false;
    for (unsigned attempt = 0; attempt < 4 && !stable; ++attempt) {
        uint32_t generation = mmio_read32(device + 0xfc);
        streams = mmio_read32(device + 0x104);
        stable = generation == mmio_read32(device + 0xfc);
    }
    if (!stable || !queue_init(CONTROL, &control) || !queue_init(TX, &tx)) return fail();
    reg(0x70, 15);
    /* The first output stream that plays S16 stereo at 48 kHz. */
    stream = UINT32_MAX;
    for (uint32_t id = 0; id < streams && id < STREAM_LIMIT && stream == UINT32_MAX; ++id) {
        request.info = (struct info_request){R_PCM_INFO, id, 1, sizeof(reply) - sizeof(reply.code)};
        if (!ask_now(sizeof(request.info), sizeof(reply))) return false;
        if (reply.direction == OUTPUT && (reply.formats[0] & (1u << FORMAT_S16)) &&
            (reply.rates[0] & (1u << RATE_48000)) && reply.channels_min <= USER_AUDIO_CHANNELS &&
            reply.channels_max >= USER_AUDIO_CHANNELS)
            stream = id;
    }
    if (stream == UINT32_MAX) return fail();
    request.params = (struct params_request){.code = R_PCM_SET_PARAMS,
                                             .stream = stream,
                                             .buffer_bytes = PERIODS * PERIOD_BYTES,
                                             .period_bytes = PERIOD_BYTES,
                                             .channels = USER_AUDIO_CHANNELS,
                                             .format = FORMAT_S16,
                                             .rate = RATE_48000};
    if (!ask_now(sizeof(request.params), sizeof(reply.code))) return false;
    request.pcm = (struct pcm_request){R_PCM_PREPARE, stream};
    if (!ask_now(sizeof(request.pcm), sizeof(reply.code))) return false;
    for (unsigned i = 0; i < PERIODS; ++i) periods[i].stream = stream;
    state = STOPPED;
    *out = (struct audio_device){.poll = poll, .latency = latency};
    return true;
}
