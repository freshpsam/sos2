#include "audio.h"
#include "platform.h"

/* A fixed table of streams, each with its queue in cached kernel pages from
 * the pool, taken at OPEN and returned at CLOSE or when the process stops.
 * Frames leave a queue as the device takes them, or, without one, as real
 * time passes: either way played counts them. Syscalls and the main loop
 * both run on CPU 0, so nothing here needs a lock. */
enum {
    STREAMS = 8,
    QUEUE_PAGES = 24,
    CAPACITY = QUEUE_PAGES * PAGE_SIZE / sizeof(struct audio_frame), /* 0.51 s */
    MIX_CHUNK = 128
};

_Static_assert(sizeof(struct audio_frame) == 2 * USER_AUDIO_CHANNELS, "SYS_AUDIO frame layout");
_Static_assert(USER_AUDIO_RATE % 1000 == 0, "the silent clock counts frames per millisecond");

struct stream {
    struct audio_frame *queue; /* NULL: free */
    unsigned pid;
    uint32_t head, count; /* the next frame to play, and how many wait */
    uint64_t played;
    bool paused;
};

static struct stream streams[STREAMS];
static struct audio_device device;
static bool online;
/* Real time in frames at the last poll: the silent clock. */
static uint64_t clock_frames;

static struct stream *stream_of(unsigned pid) {
    for (unsigned i = 0; i < STREAMS; ++i)
        if (streams[i].queue && streams[i].pid == pid) return &streams[i];
    return 0;
}

static void consume(struct stream *s, uint32_t count) {
    s->head = (s->head + count) % CAPACITY;
    s->count -= count;
    s->played += count;
}

static void close_stream(struct stream *s) {
    vm_cpu_free(s->queue, QUEUE_PAGES);
    *s = (struct stream){0};
}

void audio_init(const struct platform_devices *devices) {
#ifdef SOS2_BOARD_GTA7LITE
    (void)devices;
    online = gta7lite_audio_init(&device);
#elif defined(SOS2_BOARD_REDMI2)
    (void)devices;
    online = redmi2_audio_init(&device);
#elif defined(SOS2_DEVICE)
    (void)devices; /* the Nexus 5 has no sound driver yet */
#else
    online = virtio_snd_init(devices, &device);
#endif
}

/* Each call runs this first too: STATUS then reports the present, and frames
 * written to an empty stream are not played for time that passed before they
 * came. The present is the main loop's turn (loop_time_us), in which calls
 * run as well, so every reading of the silent clock comes from one source. */
void audio_poll(void) {
    uint64_t now = loop_time_us() * (USER_AUDIO_RATE / 1000) / 1000, due = now - clock_frames;
    clock_frames = now;
    if (online) {
        online = device.poll();
        return;
    }
    for (unsigned i = 0; i < STREAMS; ++i) {
        struct stream *s = &streams[i];
        if (s->queue && !s->paused) consume(s, s->count < due ? s->count : (uint32_t)due);
    }
}

uint32_t audio_ready(void) {
    uint32_t most = 0;
    for (unsigned i = 0; i < STREAMS; ++i)
        if (streams[i].queue && !streams[i].paused && streams[i].count > most)
            most = streams[i].count;
    return most;
}

static int16_t clip(int32_t sample) {
    return sample > INT16_MAX ? INT16_MAX : sample < INT16_MIN ? INT16_MIN : (int16_t)sample;
}

/* Sums in 32 bits, a chunk at a time, and saturates only the total. */
void audio_mix(struct audio_frame *out, uint32_t count) {
    while (count) {
        int32_t sum[2 * MIX_CHUNK] = {0};
        uint32_t n = count < MIX_CHUNK ? count : MIX_CHUNK;
        for (unsigned i = 0; i < STREAMS; ++i) {
            struct stream *s = &streams[i];
            if (!s->queue || s->paused) continue;
            uint32_t take = s->count < n ? s->count : n;
            for (uint32_t k = 0, at = s->head; k < take; ++k) {
                sum[2 * k] += s->queue[at].left;
                sum[2 * k + 1] += s->queue[at].right;
                if (++at == CAPACITY) at = 0;
            }
            consume(s, take);
        }
        for (uint32_t k = 0; k < n; ++k)
            out[k] = (struct audio_frame){clip(sum[2 * k]), clip(sum[2 * k + 1])};
        out += n;
        count -= n;
    }
}

/* Q15 gains of the volume's steps. */
static const uint16_t volume_gain[AUDIO_VOLUME_STEPS + 1] = {
    0, 260, 368, 519, 734, 1036, 1464, 2068, 2920, 4125, 5827, 8231, 11627, 16423, 23198, 32768};

void audio_mix_volume(struct audio_frame *out, uint32_t count, unsigned level, bool mono) {
    audio_mix(out, count);
    int32_t gain = volume_gain[level < AUDIO_VOLUME_STEPS ? level : AUDIO_VOLUME_STEPS];
    if (gain == volume_gain[AUDIO_VOLUME_STEPS] && !mono) return;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t left = out[i].left, right = out[i].right;
        if (mono) left = right = (left + right) >> 1;
        out[i] = (struct audio_frame){(int16_t)(left * gain >> 15), (int16_t)(right * gain >> 15)};
    }
}

/* Takes as many frames as fit, in at most two pieces around the queue's end. */
static int32_t write_frames(struct stream *s, const struct vm_space *space, user_ptr from,
                            uint32_t count) {
    const size_t frame = sizeof(struct audio_frame);
    uint32_t n = CAPACITY - s->count, tail = (s->head + s->count) % CAPACITY;
    if (n > count) n = count;
    uint32_t first = CAPACITY - tail < n ? CAPACITY - tail : n;
    if (!vm_copy_from_user(space, s->queue + tail, from, first * frame) ||
        !vm_copy_from_user(space, s->queue, from + first * frame, (n - first) * frame))
        return USER_EFAULT;
    s->count += n;
    return (int32_t)n;
}

int32_t audio_call(unsigned pid, const struct vm_space *space, unsigned op, user_ptr argument,
                   uint32_t count) {
    audio_poll();
    struct stream *s = stream_of(pid);
    if (op == USER_AUDIO_OPEN) {
        if (s) return USER_EBUSY;
        for (unsigned i = 0; i < STREAMS && !s; ++i)
            if (!streams[i].queue) s = &streams[i];
        if (!s) return USER_EAGAIN;
        struct audio_frame *queue = vm_cpu_alloc(QUEUE_PAGES);
        if (!queue) return USER_ENOMEM;
        *s = (struct stream){.queue = queue, .pid = pid};
        return 0;
    }
    if (op > USER_AUDIO_CLOSE) return USER_EINVAL;
    if (!s) return USER_EBADF;
    switch (op) {
        case USER_AUDIO_WRITE: return write_frames(s, space, argument, count);
        case USER_AUDIO_FLUSH: s->head = s->count = 0; break;
        case USER_AUDIO_PAUSE:
            if (argument > 1) return USER_EINVAL;
            s->paused = argument;
            break;
        case USER_AUDIO_STATUS: {
            struct user_audio_status status = {.played = s->played,
                                               .queued = s->count,
                                               .capacity = CAPACITY,
                                               .latency = online ? device.latency() : 0,
                                               .flags = online ? USER_AUDIO_DEVICE : 0};
            if (!vm_copy_to_user(space, argument, &status, sizeof(status))) return USER_EFAULT;
            break;
        }
        default: close_stream(s); break;
    }
    return 0;
}

void audio_close_process(unsigned pid) {
    struct stream *s = stream_of(pid);
    if (s) close_stream(s);
}

bool audio_process_playing(unsigned pid) {
    const struct stream *s = stream_of(pid);
    return s && !s->paused && s->count;
}
