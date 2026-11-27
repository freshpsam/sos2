#include "pc.h"
#include "audio.h"
#include "platform.h"
#include "user_abi.h"

/* Intel High Definition Audio on a PC, polled: the controller reset, its
 * command and response rings (CORB, RIRB) for the codec's verbs, and the
 * first codec's audio function walked from an output pin (line out, else a
 * speaker or headphones) back to a DAC, its selectors set and amplifiers
 * unmuted at 0 dB; of several controllers, the first whose codec has such a
 * path (a graphics card's, HDMI only, has none). Playback is one output
 * stream, 48 kHz stereo 16-bit,
 * over a 256 ms ring in RAM that the stream's DMA reads through a buffer
 * descriptor list, as boards/redmi2/audio.c keeps its own: written ahead
 * of the position (LPIB), silence behind, stopped after 3 s without audio
 * and when the DMA stalls.
 * https://www.intel.com/content/www/us/en/standards/high-definition-audio-specification.html
 * (HD Audio 1.0a: 3.3, the controller's registers; 4.4, the rings; 7.3,
 * the codec's verbs and parameters) */
enum {
    GCAP = 0x00,
    GCTL = 0x08,
    STATESTS = 0x0e,
    CORBLBASE = 0x40,
    CORBUBASE = 0x44,
    CORBWP = 0x48,
    CORBRP = 0x4a,
    CORBCTL = 0x4c,
    CORBSIZE = 0x4e,
    RIRBLBASE = 0x50,
    RIRBUBASE = 0x54,
    RIRBWP = 0x58,
    RINTCNT = 0x5a,
    RIRBCTL = 0x5c,
    RIRBSTS = 0x5d,
    RIRBSIZE = 0x5e,
    SD_CTL = 0x00,
    SD_STS = 0x03,
    SD_LPIB = 0x04,
    SD_CBL = 0x08,
    SD_LVI = 0x0c,
    SD_FMT = 0x12,
    SD_BDPL = 0x18,
    SD_BDPU = 0x1c,
    STREAM_TAG = 1,
    FORMAT = 0x0011, /* 48 kHz, 16 bits, 2 channels */
    RING = 12288,
    GUARD = 256,
    LEAD = RING - 2 * GUARD,
    STALL_US = 500000,
    IDLE_US = 3000000,
    LAP_US = RING * 1000000ull / USER_AUDIO_RATE,
    MIX_FRAMES = 512,
    VOLUME = 12,
};

/* Verbs: 12 bits with an 8-bit payload, or 4 with a 16-bit one. */
#define PARAMETER(p)        (0xf0000u | (p))
#define CONNECTIONS(i)      (0xf0200u | (i))
#define SELECT(i)           (0x70100u | (i))
#define POWER_D0            0x70500u
#define STREAM(tag)         (0x70600u | (tag) << 4)
#define PIN_CONTROL(v)      (0x70700u | (v))
#define EAPD                0x70c02u
#define CONFIG_DEFAULT      0xf1c00u
#define CONVERTER_FORMAT(f) (0x20000u | (f))
#define AMPLIFIER(v)        (0x30000u | (v))

struct bdl_entry {
    uint64_t address;
    uint32_t length, flags;
};

static uint32_t corb[256] __attribute__((aligned(1024)));
static uint64_t rirb[256] __attribute__((aligned(2048)));
static struct bdl_entry bdl[2] __attribute__((aligned(128)));
static uint32_t ring[RING] __attribute__((aligned(4096)));
static const struct pci_function *controller;
static uintptr_t base, stream;
static unsigned corb_wp, rirb_rp, codec, dac;

static enum { OFF, ON, FAILED } state;

static uint64_t played, written, cleared, moved_at, heard_at, polled_at;
static uint32_t read_at;

/* Every controller's window: a PC's first is often a graphics card's, whose
 * codecs only drive HDMI. */
#define CONTROLLERS 4
static const struct pci_function *candidates[CONTROLLERS];
static unsigned candidate_count;

void hda_probe(struct platform_devices *d) {
    candidate_count = 0;
    for (unsigned i = 0; candidate_count < CONTROLLERS; ++i) {
        const struct pci_function *f = pci_find(0x040300, 0xffff00, i);
        if (!f) break;
        if (pci_map(d, f, 0)) candidates[candidate_count++] = f;
    }
}

static bool wait8(uintptr_t at, uint8_t mask, uint8_t value) {
    for (uint64_t deadline = time_us() + 100000; (mmio_read8(at) & mask) != value;)
        if (time_us() > deadline) return false;
    return true;
}

/* A verb to a node of the codec, and its response; the response flag
 * (raised for every response, RINTCNT being 1, but interrupting nothing:
 * INTCTL stays clear) cleared after it, without which QEMU's controller
 * sends no more. */
static bool verb(unsigned node, uint32_t command, uint32_t *response) {
    corb_wp = (corb_wp + 1) % 256;
    corb[corb_wp] = (uint32_t)codec << 28 | (uint32_t)node << 20 | command;
    barrier();
    mmio_write16(base + CORBWP, (uint16_t)corb_wp);
    for (uint64_t deadline = time_us() + 10000; (mmio_read16(base + RIRBWP) & 0xff) == rirb_rp;)
        if (time_us() > deadline) return false;
    rirb_rp = (rirb_rp + 1) % 256;
    barrier();
    if (response) *response = (uint32_t)rirb[rirb_rp];
    mmio_write8(base + RIRBSTS, 5);
    return true;
}

static uint32_t parameter(unsigned node, unsigned which) {
    uint32_t value = 0;
    return verb(node, PARAMETER(which), &value) ? value : 0;
}

/* A widget's output amplifier, where it has one, at 0 dB and unmuted; an
 * input's, at index, likewise. */
static void unmute(unsigned node, uint32_t caps, unsigned afg, bool input, unsigned index) {
    if (!(caps & (input ? 2 : 4))) return;
    uint32_t amp = parameter(caps & 8 ? node : afg, input ? 0x0d : 0x12);
    uint32_t gain = amp & 0x7f; /* the 0 dB step */
    (void)verb(node, AMPLIFIER((input ? 0x7000u : 0xb000u) | index << 8 | gain), 0);
}

/* The node's connection list entry at index (short form only). */
static unsigned connection(unsigned node, unsigned index) {
    uint32_t entries = 0;
    if (!verb(node, CONNECTIONS(index & ~3u), &entries)) return 0;
    return entries >> (8 * (index & 3)) & 0xff;
}

/* From node, through selectors and mixers, to a DAC: each step's input
 * selected and unmuted; the DAC found, or 0. */
static unsigned find_dac(unsigned node, unsigned afg, unsigned depth) {
    uint32_t caps = parameter(node, 0x09), type = caps >> 20 & 15;
    if (type == 0) return node;
    if (depth > 4) return 0;
    uint32_t list = parameter(node, 0x0e);
    if (list & 0x80) return 0; /* the long form: no codec SOS2 drives has it */
    for (unsigned i = 0; i < (list & 0x7f); ++i) {
        unsigned next = connection(node, i), found = next ? find_dac(next, afg, depth + 1) : 0;
        if (!found) continue;
        if (type == 3 || type == 4) (void)verb(node, SELECT(i), 0);
        unmute(node, caps, afg, true, i);
        unmute(node, caps, afg, false, 0);
        (void)verb(node, POWER_D0, 0);
        return found;
    }
    return 0;
}

/* The codec's audio function, powered, and an output path to a DAC: line
 * out first, then a speaker, then headphones, among pins that connect. */
static bool codec_setup(void) {
    uint32_t nodes = parameter(0, 0x04), afg = 0;
    for (unsigned n = nodes >> 16 & 0xff; n < (nodes >> 16 & 0xff) + (nodes & 0xff); ++n)
        if ((parameter(n, 0x05) & 0xff) == 1) afg = n;
    if (!afg) return false;
    (void)verb(afg, POWER_D0, 0);
    uint32_t widgets = parameter(afg, 0x04);
    unsigned first = widgets >> 16 & 0xff, count = widgets & 0xff;
    for (unsigned preference = 0; preference < 3 && !dac; ++preference)
        for (unsigned n = first; n < first + count && !dac; ++n) {
            uint32_t caps = parameter(n, 0x09), config = 0;
            if ((caps >> 20 & 15) != 4 || !(parameter(n, 0x0c) & 0x10) ||
                !verb(n, CONFIG_DEFAULT, &config) || (config >> 30) == 1 ||
                (config >> 20 & 15) != preference)
                continue;
            dac = find_dac(n, afg, 0);
            if (!dac) continue;
            (void)verb(n, PIN_CONTROL(preference == 2 ? 0xc0u : 0x40u), 0);
            if (parameter(n, 0x0c) & 0x10000) (void)verb(n, EAPD, 0);
            (void)verb(dac, POWER_D0, 0);
            unmute(dac, parameter(dac, 0x09), afg, false, 0);
        }
    return dac != 0;
}

static void ring_write(uint64_t at, const struct audio_frame *frames, uint32_t count) {
    uint32_t index = (uint32_t)(at % RING);
    for (uint32_t i = 0; i < count; ++i) {
        ring[index] =
            frames ? (uint16_t)frames[i].left | (uint32_t)(uint16_t)frames[i].right << 16 : 0;
        if (++index == RING) index = 0;
    }
}

static void mix(uint64_t at, uint32_t count) {
    static struct audio_frame frames[MIX_FRAMES];
    while (count) {
        uint32_t n = count < MIX_FRAMES ? count : MIX_FRAMES;
        audio_mix_volume(frames, n, VOLUME, false);
        ring_write(at, frames, n);
        at += n;
        count -= n;
    }
}

static void power_down(void) {
    mmio_write8(stream + SD_CTL, 0);
    state = OFF;
}

/* The stream reset and started over the silent ring; the DAC on it. */
static bool power_up(void) {
    ring_write(0, 0, RING);
    mmio_write8(stream + SD_CTL, 0);
    mmio_write8(stream + SD_CTL, 1);
    if (!wait8(stream + SD_CTL, 1, 1)) return false;
    mmio_write8(stream + SD_CTL, 0);
    if (!wait8(stream + SD_CTL, 1, 0)) return false;
    mmio_write8(stream + SD_STS, 0x1c);
    for (unsigned i = 0; i < 2; ++i)
        bdl[i] = (struct bdl_entry){(uintptr_t)&ring[i * RING / 2], RING / 2 * sizeof(ring[0]), 0};
    barrier();
    mmio_write32(stream + SD_BDPL, (uint32_t)(uintptr_t)bdl);
    mmio_write32(stream + SD_BDPU, (uint32_t)((uint64_t)(uintptr_t)bdl >> 32));
    mmio_write32(stream + SD_CBL, sizeof(ring));
    mmio_write16(stream + SD_LVI, 1);
    mmio_write16(stream + SD_FMT, FORMAT);
    mmio_write8(stream + SD_CTL + 2, STREAM_TAG << 4);
    if (!verb(dac, CONVERTER_FORMAT(FORMAT), 0) || !verb(dac, STREAM(STREAM_TAG), 0)) return false;
    played = written = cleared = 0;
    read_at = 0;
    moved_at = heard_at = polled_at = time_us();
    mmio_write8(stream + SD_CTL, 2);
    state = ON;
    return true;
}

static uint32_t latency(void) {
    return state == ON && written > played ? (uint32_t)(written - played) : 0;
}

/* As boards/redmi2/audio.c's poll: the DMA's progress from its position,
 * audio from where the last ended (or past the DMA if it ran out), and
 * silence a lap ahead behind it. */
static bool poll(void) {
    if (state == FAILED) return false;
    uint32_t ready = audio_ready();
    if (state == OFF) {
        if (!ready) return true;
        if (!power_up()) {
            power_down();
            state = FAILED;
            return false;
        }
    }
    uint64_t now = time_us();
    uint32_t at = (mmio_read32(stream + SD_LPIB) / sizeof(ring[0])) % RING;
    bool lapped = now - polled_at >= LAP_US;
    polled_at = now;
    if (at != read_at || lapped) {
        uint64_t moved = (at + RING - read_at) % RING;
        uint64_t due = (now - moved_at) * USER_AUDIO_RATE / 1000000;
        if (due > moved + RING / 2) moved += (due - moved + RING / 2) / RING * RING;
        played += moved;
        read_at = at;
        moved_at = now;
    } else if (now - moved_at >= STALL_US) {
        power_down();
        state = FAILED;
        return false;
    }
    if (ready) {
        uint64_t start = written > played + GUARD ? written : played + GUARD;
        if (cleared < played) cleared = played;
        if (cleared < start) {
            ring_write(cleared, 0, (uint32_t)(start - cleared));
            cleared = start;
        }
        if (start < played + LEAD) {
            uint32_t room = (uint32_t)(played + LEAD - start), n = ready < room ? ready : room;
            mix(start, n);
            written = start + n;
        }
    }
    if (written > played) heard_at = now;
    if (cleared < written) cleared = written;
    if (cleared < played) cleared = played;
    if (cleared < played + RING) {
        ring_write(cleared, 0, (uint32_t)(played + RING - cleared));
        cleared = played + RING;
    }
    barrier();
    if (!ready && written <= played && now - heard_at >= IDLE_US) power_down();
    return true;
}

/* The controller out of reset, its codecs reported, the rings running and
 * the first codec's output path set up. */
static bool setup(void) {
    pci_enable(controller);
    base = (uintptr_t)controller->bar[0];
    mmio_write32(base + GCTL, mmio_read32(base + GCTL) & ~1u);
    for (uint64_t deadline = time_us() + 100000; mmio_read32(base + GCTL) & 1;)
        if (time_us() > deadline) return false;
    delay_us(100);
    mmio_write32(base + GCTL, mmio_read32(base + GCTL) | 1);
    for (uint64_t deadline = time_us() + 100000; !(mmio_read32(base + GCTL) & 1);)
        if (time_us() > deadline) return false;
    delay_us(1000);
    uint16_t present = mmio_read16(base + STATESTS);
    if (!present) return false;
    codec = (unsigned)__builtin_ctz(present);
    stream = base + 0x80 + 0x20 * (mmio_read16(base + GCAP) >> 8 & 15);
    mmio_write8(base + CORBCTL, 0);
    mmio_write8(base + RIRBCTL, 0);
    if (!wait8(base + CORBCTL, 2, 0) || !wait8(base + RIRBCTL, 2, 0)) return false;
    mmio_write8(base + CORBSIZE, 2);
    mmio_write8(base + RIRBSIZE, 2);
    mmio_write32(base + CORBLBASE, (uint32_t)(uintptr_t)corb);
    mmio_write32(base + CORBUBASE, (uint32_t)((uint64_t)(uintptr_t)corb >> 32));
    mmio_write32(base + RIRBLBASE, (uint32_t)(uintptr_t)rirb);
    mmio_write32(base + RIRBUBASE, (uint32_t)((uint64_t)(uintptr_t)rirb >> 32));
    mmio_write16(base + CORBRP, 0x8000);
    for (uint64_t deadline = time_us() + 10000; !(mmio_read16(base + CORBRP) & 0x8000);)
        if (time_us() > deadline) break; /* some controllers never show it */
    mmio_write16(base + CORBRP, 0);
    mmio_write16(base + CORBWP, 0);
    mmio_write16(base + RIRBWP, 0x8000);
    mmio_write16(base + RINTCNT, 1);
    mmio_write8(base + RIRBSTS, 5);
    corb_wp = 0;
    rirb_rp = 0;
    mmio_write8(base + CORBCTL, 2);
    mmio_write8(base + RIRBCTL, 3);
    return codec_setup();
}

/* The first controller whose first codec has an output path; the others
 * have their rings stopped and stay in reset. Silent until a stream plays. */
bool hda_init(struct audio_device *out) {
    state = FAILED;
    for (unsigned i = 0; i < candidate_count; ++i) {
        controller = candidates[i];
        base = 0;
        dac = 0;
        if (setup()) {
            state = OFF;
            *out = (struct audio_device){.poll = poll, .latency = latency};
            return true;
        }
        if (!base) continue;
        mmio_write8(base + CORBCTL, 0);
        mmio_write8(base + RIRBCTL, 0);
        (void)wait8(base + CORBCTL, 2, 0);
        (void)wait8(base + RIRBCTL, 2, 0);
        mmio_write32(base + GCTL, mmio_read32(base + GCTL) & ~1u);
    }
    controller = 0;
    return false;
}
