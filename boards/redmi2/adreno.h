#ifndef SOS2_ADRENO_H
#define SOS2_ADRENO_H
#include "graphics.h"

/* A single kernel-owned A306 compute queue. No userspace command submission.
 * All GPU addresses refer only to dedicated uncached pages, never scanout,
 * page tables, a process, firmware, or any other kernel allocation. */
#define ADRENO_ARENA_SIZE   0x900000u
#define ADRENO_IOVA         0x10000000u
#define ADRENO_RING         0x8000u
#define ADRENO_RING_WORDS   8192u
#define ADRENO_FENCE        0x10000u
#define ADRENO_SHADER       0x11000u
#define ADRENO_COPY_SHADER  (ADRENO_SHADER + 0xc00u)
#define ADRENO_PRIVATE      0x20000u
#define ADRENO_PRIVATE_SIZE 0x102000u
#define ADRENO_SOURCE       0x140000u
#define ADRENO_SOURCE_SIZE  (480u * 800u * 4u)
#define ADRENO_OUTPUT       0x300000u
#define ADRENO_OUTPUT_MAX   (720u * 1280u * 4u)
#define ADRENO_UPLOAD       0x700000u
#define ADRENO_UPLOAD_SIZE  ADRENO_SOURCE_SIZE
#define ADRENO_PATCHES      16u

struct adreno_rect {
    unsigned x, y, width, height;
};

struct adreno_copy {
    unsigned sx, sy, dx, dy, width, height;
};

struct adreno_io {
    void *context;
    uint32_t (*read)(void *, uintptr_t);
    void (*write)(void *, uintptr_t, uint32_t);
    uint64_t (*now)(void *);
    void (*barrier)(void *);
    bool (*restore)(void *, unsigned context_bank);
};

struct adreno_device {
    struct adreno_io io;
    unsigned char *memory;
    const unsigned char *pm4, *pfp;
    unsigned pm4_size, pfp_size;
    unsigned ring_write, sequence, shader_words, stride, bytes;
    enum pixel_format format;
    /* powered: the OXILI power domain is on (some clocks may be off);
     * clocked: all five clocks too, so GPU registers may be touched;
     * quarantined: the GPU was powered when it failed, so its arena is never
     * reused. */
    bool powered, clocked, running, pending, quarantined, attempted;
    bool collapsed; /* adreno_power_off took the power domain down. */
    uint64_t deadline;
    unsigned polls;
    unsigned x, y, width, height;
};

bool adreno_firmware_valid(const void *pm4, unsigned pm4_size, const void *pfp, unsigned pfp_size);
bool adreno_init(struct adreno_device *, const struct adreno_io *, void *arena, const void *pm4,
                 unsigned pm4_size, const void *pfp, unsigned pfp_size, enum pixel_format,
                 unsigned stride);
/* An idle GPU's power domain off, as Linux's slumber, and back on with its
 * engine restarted from the firmware given at init; false if the GPU is not
 * idle (it runs on) or does not come back (it is quarantined). */
bool adreno_power_off(struct adreno_device *);
bool adreno_power_on(struct adreno_device *);
/* One bounded batch: overlap-safe retained copy, immutable upload patches,
 * then scaling. Only fixed arena buffers are addressable. Patch rectangles
 * use 16x8 alignment; copy origins may be arbitrary, extents must be aligned. */
bool adreno_frame(struct adreno_device *, const struct adreno_copy *,
                  const struct adreno_rect *patches, unsigned count, struct adreno_rect output);
/* 0 pending, 1 completed/idle, -1 permanently disabled. Never blocks. */
int adreno_poll(struct adreno_device *);
void adreno_disable(struct adreno_device *);
/* KGSL's nap for an idle GPU: its core and GMEM clocks stop while the power
 * domain keeps the GPU's state, and restart before the next job. Waking
 * returns whether the clocks run again. */
bool adreno_nap(struct adreno_device *, bool nap);
bool adreno_selftest(struct adreno_device *);
#endif
