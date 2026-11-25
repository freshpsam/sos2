#include "audio.h"
#include "nexus5.h"
#include "platform.h"

/* The Nexus 5's loudspeaker, played as the stock system plays it: PCM
 * through the audio DSP (adsp.c, apr.c, q6audio.c) to the WCD9320 codec over
 * SLIMbus (qmi.c, slimbus.c, wcd9320.c), whose own class-D driver feeds the
 * speaker (the mixer path "speaker": SLIM RX1 into RX7, its compander, the
 * speaker driver).
 *
 * The DSP's firmware loads once, after boot, on the service worker a step
 * at a time (its 9 MB come from Android's system partition); its links come
 * up from the main loop. Until then, and while sound is off, the streams
 * play silently in real time, as without a device. When a stream has frames
 * the path powers up: the bus, the codec's speaker route, the DSP's port,
 * route and stream; then 20 ms periods of the mix (mono, at the volume the
 * keys set) go to the DSP from a ring in uncached RAM, each written again
 * once the DSP has read it, with silence where the streams run short; the
 * DSP holds 240 ms of them. Three seconds after the last frame it all powers
 * down again. A failure powers down what came up and tries again 10 s
 * later; after three in a row, the streams play silently until a restart. */
enum {
    PERIOD = 960, /* frames: 20 ms */
    PERIODS = 16, /* the ring: 320 ms */
    QUEUED = 12,  /* periods the DSP holds at once: 240 ms */
    PERIOD_BYTES = PERIOD * 2,
    RING_BYTES = PERIODS * PERIOD_BYTES,
    MAP_BYTES = (RING_BYTES + 4095) & ~4095,
    PIPELINE = PERIOD, /* the DSP's own delay after it has read a period */
    IDLE_US = 3000000,
    LINK_US = 10000000,
    STALL_US = 500000,
    RETRY_US = 10000000,
    ATTEMPTS = 3,
    SPEAKER_VOLUME = 12
};

/* Uncached and at its physical address, as the DSP reads it. */
static int16_t ring[MAP_BYTES / 2] __attribute__((aligned(4096)));

static enum { LOADING, LINKING, IDLE, PLAYING, FAILED } state;

static unsigned volume = SPEAKER_VOLUME, failures;
static uint64_t linking_since, silent_at, heard_at, done_at, retry_at;
/* Periods written to the DSP and periods it has read, counted from the
 * stream's start; the next period's slot is written % PERIODS. */
static uint64_t written, done;

/* The streams' frames as real time passes, unheard. */
static void silent(void) {
    static struct audio_frame scratch[256] __attribute__((section(".cpu_private")));
    uint64_t now = time_us(), due = (now - silent_at) * USER_AUDIO_RATE / 1000000;
    if (!due) return;
    silent_at += due * 1000000 / USER_AUDIO_RATE;
    while (due) {
        uint32_t n = due < 256 ? (uint32_t)due : 256;
        audio_mix(scratch, n);
        due -= n;
    }
}

static bool pending(void) { return state == LOADING; }

/* The firmware's steps, on the service worker. */
static void service(void) {
    const char *error = 0;
    bool finished = nexus5_adsp_load_step(&error);
    if (error) state = FAILED;
    else if (finished) {
        state = LINKING;
        linking_since = time_us();
    }
}

/* The DSP's APR channel open and its SLIMbus service listed. */
static bool linked(void) {
    bool apr = nexus5_apr_poll();
    return nexus5_qmi_has(0x301, 1) && apr;
}

/* One period of the mix, mono at the volume, into its slot. */
static void fill(uint64_t period) {
    static struct audio_frame frames[PERIOD] __attribute__((section(".cpu_private")));
    int16_t *slot = ring + (period % PERIODS) * PERIOD;
    if (!audio_ready()) {
        memset(slot, 0, PERIOD_BYTES);
        return;
    }
    heard_at = time_us();
    audio_mix_volume(frames, PERIOD, volume, true);
    for (unsigned i = 0; i < PERIOD; ++i) slot[i] = frames[i].left;
}

static void period_done(uint32_t token) {
    (void)token;
    if (done < written) {
        ++done;
        done_at = time_us();
    }
}

/* Periods for the DSP up to what it holds. One written to a DSP that had
 * run dry restarts the stall watch: nothing was due from it meanwhile. */
static bool queue(void) {
    while (written - done < QUEUED) {
        if (written == done) done_at = time_us();
        fill(written);
        barrier();
        uint32_t at = (uint32_t)(uintptr_t)ring + (uint32_t)(written % PERIODS) * PERIOD_BYTES;
        if (!nexus5_q6_write(at, PERIOD_BYTES, (uint32_t)written)) return false;
        ++written;
    }
    return true;
}

static void power_down(void) {
    (void)nexus5_q6_close();
    (void)nexus5_speaker(false);
    state = IDLE;
    silent_at = time_us();
}

/* What came up powered down again; another try later, unless this was the
 * last. */
static bool fail(void) {
    power_down();
    retry_at = time_us() + RETRY_US;
    if (++failures < ATTEMPTS) return true;
    state = FAILED;
    return false;
}

static bool power_up(void) {
    if (nexus5_speaker(true)) return fail();
    written = done = 0;
    if (!nexus5_q6_map((uint32_t)(uintptr_t)ring, MAP_BYTES) || !nexus5_q6_open()) return fail();
    heard_at = done_at = time_us();
    if (!queue() || !nexus5_q6_run(period_done)) return fail();
    state = PLAYING;
    failures = 0;
    return true;
}

static uint32_t latency(void) {
    if (state != PLAYING) return 0;
    uint64_t held = (written - done) * PERIOD + PIPELINE;
    /* The period the DSP reads now, as far as time has gone into it. */
    uint64_t into = (time_us() - done_at) * USER_AUDIO_RATE / 1000000;
    if (into > PERIOD) into = PERIOD;
    return (uint32_t)(held > into ? held - into : 0);
}

static bool poll(void) {
    switch (state) {
        case FAILED: return false;
        case LOADING: silent(); return true;
        case LINKING:
            silent();
            if (linked()) state = IDLE;
            else if (time_us() - linking_since >= LINK_US) state = FAILED;
            return state != FAILED;
        case IDLE:
            (void)nexus5_apr_poll();
            nexus5_qmi_poll();
            if (!audio_ready() || time_us() < retry_at) {
                silent();
                return true;
            }
            return power_up();
        case PLAYING:
            if (!nexus5_apr_poll() || !queue() || time_us() - done_at >= STALL_US) return fail();
            if (!audio_ready() && time_us() - heard_at >= IDLE_US) power_down();
            return true;
    }
    return false;
}

unsigned nexus5_audio_volume(int step) {
    if (step > 0 && volume < AUDIO_VOLUME_STEPS) ++volume;
    if (step < 0 && volume) --volume;
    return volume;
}

bool nexus5_audio_init(struct audio_device *out) {
    state = LOADING;
    silent_at = time_us();
    *out = (struct audio_device){
        .poll = poll, .latency = latency, .pending = pending, .service = service};
    return true;
}
