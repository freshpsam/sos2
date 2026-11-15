/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2013 Red Hat; Rob Clark
 * Copyright (c) 2014 The Linux Foundation. All rights reserved.
 * MSM8916 / Adreno 306 bounded compute painting and presentation.
 * Hardware sequences: Linux v6.12 a3xx_gpu.c, gcc-msm8916.c, qcom_iommu.c;
 * compute state: Freedreno fdre-a3xx/program.c and freedreno.c.
 * See adreno-notices.txt and docs/gpu.md for provenance and limits. */
#include "adreno.h"
#include "platform.h"
#include "adreno_shaders.h"

#define GCC        0x01800000u
#define GPU        0x01c00000u
#define SMMU       0x01f08000u
#define GPU_ERRORS 0x03210f7eu
#define TIMEOUT_US 100000u

static uint32_t read32(struct adreno_device *g, uintptr_t a) {
    return g->io.read(g->io.context, a);
}

static void write32(struct adreno_device *g, uintptr_t a, uint32_t v) {
    g->io.write(g->io.context, a, v);
}

static void sync(struct adreno_device *g) { g->io.barrier(g->io.context); }

static uint64_t now(struct adreno_device *g) { return g->io.now(g->io.context); }

static void reg(struct adreno_device *g, unsigned r, uint32_t v) { write32(g, GPU + 4 * r, v); }

static void bits(struct adreno_device *g, uintptr_t a, uint32_t mask, uint32_t value) {
    write32(g, a, (read32(g, a) & ~mask) | value);
}

static bool wait_bits(struct adreno_device *g, uintptr_t a, uint32_t mask, uint32_t value) {
    uint64_t start = now(g);
    for (unsigned i = 0; i < 100000; ++i) {
        if ((read32(g, a) & mask) == value) return true;
        if (now(g) - start >= 10000) break;
    }
    return false;
}

static uint32_t crc(const void *bytes, unsigned n) {
    const unsigned char *p = bytes;
    uint32_t c = UINT32_MAX;
    while (n--) {
        c ^= *p++;
        for (unsigned i = 0; i < 8; ++i) c = (c >> 1) ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}

bool adreno_firmware_valid(const void *pm4, unsigned n, const void *pfp, unsigned m) {
    /* This is an integrity/compatibility check, not a signature verifier.
     * Firmware comes only from the trusted boot bundle, never a web process. */
    return pm4 && pfp && n == 9220 && m == 1156 && crc(pm4, n) == 0xe68e4d55 &&
           crc(pfp, m) == 0x0181387e;
}

/* The GPU's clocks in the vendor's slumber order (bimc_gpu, bimc_gfx, gmem,
 * ahb, gfx3d). */
static const unsigned branches[] = {0x31040, 0x31024, 0x59024, 0x59028, 0x59020};

/* A failed or faulted GPU stays off for the boot: interrupts off, the
 * microengine halted and the core held in reset, so no DMA runs (only with
 * every clock running: KGSL never touches the GPU's registers otherwise);
 * then its clocks off and its power domain collapsed, as at slumber, so it
 * costs nothing in standby. A domain that will not collapse stays on. */
void adreno_disable(struct adreno_device *g, enum adreno_error error) {
    if (!g) return;
    if (g->error == ADRENO_OK) g->error = error;
    g->running = false;
    g->pending = false;
    g->collapsed = false;
    if (g->clocked) {
        reg(g, 0x0063, 0);
        reg(g, 0x01f6, 1u << 28); /* interrupts off; ME halt */
        reg(g, 0x0018, 1);
        sync(g); /* Keep reset asserted. No uncertain DMA reuse. */
    }
    g->clocked = false;
    if (g->powered) {
        g->powered = false;
        g->quarantined = true;
        for (unsigned i = 0; i < 5; ++i) {
            bits(g, GCC + branches[i], 1, 0);
            (void)wait_bits(g, GCC + branches[i], 1u << 31, 1u << 31);
        }
        bits(g, GCC + 0x5901c, 1, 1);
        (void)wait_bits(g, GCC + 0x5901c, 1u << 31, 0);
    }
}

static bool fail(struct adreno_device *g, enum adreno_error e) {
    adreno_disable(g, e);
    return false;
}

static bool clock_branch(struct adreno_device *g, unsigned offset) {
    bits(g, GCC + offset, 1, 1);
    return wait_bits(g, GCC + offset, 1u << 31, 0);
}

/* msm-3.10 kgsl_pwrctrl.c's NAP state disables the GPU's clocks with the
 * OXILI power domain on and enables them again without reinitialising the
 * GPU. Only the core and GMEM clocks stop here: the register and bus clocks
 * stay on. Until both run again, nothing touches the GPU's registers. */
bool adreno_nap(struct adreno_device *g, bool nap) {
    if (!g || !g->powered || g->quarantined) return false;
    if (nap) {
        g->clocked = false;
        bits(g, GCC + 0x59020, 1, 0);
        bits(g, GCC + 0x59024, 1, 0);
        sync(g);
        (void)wait_bits(g, GCC + 0x59020, 1u << 31, 1u << 31);
        return true;
    }
    g->clocked = clock_branch(g, 0x59024) && clock_branch(g, 0x59020);
    return g->clocked;
}

static bool engine(struct adreno_device *g);

static bool clocks(struct adreno_device *g) {
    /* Do not retune a shared PLL or change voltage. GPLL0 is the existing
     * 800 MHz source; dedicated GPU roots run at a conservative 200 MHz. */
    if (!(read32(g, GCC + 0x2101c) & (1u << 17))) return fail(g, ADRENO_CLOCK);
    bits(g, GCC + 0x45000, 1, 1);
    bits(g, GCC + 0x4500c, (1u << 12) | (1u << 2), (1u << 12) | (1u << 2));
    if (!wait_bits(g, GCC + 0x12038, 1u << 31, 0) || !wait_bits(g, GCC + 0x12020, 1u << 31, 0))
        return fail(g, ADRENO_CLOCK);
    /* OXILI GDSC: software control, with the waits Linux's gdsc.c and the
     * vendor's set (EN_REST 2, EN_FEW 8, CLK_DIS 2): EN_FEW paces the
     * power switches against inrush on the core rail. No clock starts
     * within 1 us of the domain's power (gdsc_enable). */
    bits(g, GCC + 0x5901c, 0x00fff007, 0x00282000);
    /* From here a failure turns the domain and its clocks off again. */
    g->powered = true;
    if (!wait_bits(g, GCC + 0x5901c, 1u << 31, 1u << 31)) return fail(g, ADRENO_POWER);
    uint64_t settle = now(g);
    for (unsigned i = 0; i < 1000 && now(g) - settle < 2; ++i) { }
    write32(g, GCC + 0x59004, 0x307);
    bits(g, GCC + 0x59000, 1, 1);
    write32(g, GCC + 0x3102c, 0x107);
    bits(g, GCC + 0x31028, 1, 1);
    if (!wait_bits(g, GCC + 0x59000, 1, 0) || !wait_bits(g, GCC + 0x31028, 1, 0))
        return fail(g, ADRENO_CLOCK);
    if (!clock_branch(g, 0x59028) || !clock_branch(g, 0x31024) || !clock_branch(g, 0x31040) ||
        !clock_branch(g, 0x59024) || !clock_branch(g, 0x59020))
        return fail(g, ADRENO_CLOCK);
    g->clocked = true;
    /* Never leave an inherited command queue running while replacing tables. */
    reg(g, 0x01f6, 1u << 28);
    reg(g, 0x0018, 1);
    sync(g);
    return true;
}

static bool map(struct adreno_device *g, unsigned offset, unsigned size, bool writable) {
    if ((offset & 4095) || (size & 4095) || !size || offset > ADRENO_ARENA_SIZE - size)
        return false;
    uint64_t *leaves = (uint64_t *)(g->memory + 0x2000);
    for (unsigned i = offset / 4096; i < (offset + size) / 4096; ++i) {
        if (leaves[i]) return false;
        /* Stage-1 LPAE, normal uncached, inner-shareable, AF, nG, PXN/UXN.
         * GPU user and privileged context banks share only this allowlist. */
        leaves[i] = ((uintptr_t)g->memory + i * 4096u) | UINT64_C(0x0060000000000f43) |
                    (writable ? 0 : 0x80);
    }
    return true;
}

static bool banks(struct adreno_device *g);

static bool iommu(struct adreno_device *g) {
    uint64_t *root = (uint64_t *)g->memory, *middle = (uint64_t *)(g->memory + 0x1000);
    root[0] = (uintptr_t)middle | 3;
    _Static_assert(0x2000 + ((ADRENO_ARENA_SIZE + 0x1fffff) / 0x200000) * 4096 <= ADRENO_RING,
                   "page table space");
    for (unsigned i = 0; i < (ADRENO_ARENA_SIZE + 0x1fffff) / 0x200000; ++i)
        middle[(ADRENO_IOVA >> 21) + i] = (uintptr_t)(g->memory + 0x2000 + i * 4096) | 3;
    unsigned out_size = (g->stride * 1280u + 4095) & ~4095u;
    if (!map(g, ADRENO_RING, 32768, false) || !map(g, ADRENO_FENCE, 4096, true) ||
        !map(g, ADRENO_SHADER, 4096, false) || !map(g, ADRENO_PRIVATE, ADRENO_PRIVATE_SIZE, true) ||
        !map(g, ADRENO_SOURCE, ADRENO_SOURCE_SIZE, true) ||
        !map(g, ADRENO_OUTPUT, out_size, true) || !map(g, ADRENO_UPLOAD, ADRENO_UPLOAD_SIZE, false))
        return fail(g, ADRENO_MMU);
    sync(g);
    return banks(g);
}

/* The SMMU's two GPU context banks, outside the GPU's power domain: their
 * secure configuration restored, then the arena's tables. */
static bool banks(struct adreno_device *g) {
    uint64_t *root = (uint64_t *)g->memory;
    for (unsigned cb = 1; cb <= 2; ++cb) {
        if (!g->io.restore(g->io.context, cb)) return fail(g, ADRENO_SCM);
        uintptr_t a = SMMU + cb * 4096;
        write32(g, a, 0);
        write32(g, a + 0x58, 0xc00001fe);
        write32(g, a + 0x08, 1);
        write32(g, a + 0x20, (uintptr_t)root);
        write32(g, a + 0x24, cb << 16);
        write32(g, a + 0x28, 0);
        write32(g, a + 0x2c, 0);
        write32(g, a + 0x10, 0x38002);    /* upstream SEP, 40-bit output, ARM32 LPAE */
        write32(g, a + 0x30, 0x80802020); /* EAE, EPD1, T0SZ=32, outer-shareable NC walk */
        write32(g, a + 0x38, 0x44);
        write32(g, a + 0x3c, 0);
        write32(g, a + 0x34, cb);
        write32(g, a + 0x610, cb);
        write32(g, a + 0x7f0, 0);
        if (!wait_bits(g, a + 0x7f4, 1, 0)) return fail(g, ADRENO_MMU);
        write32(g, a, 0x10a7); /* ASIDPNE, CFCFG, CFRE, AFE, TRE, M; polled */
        sync(g);
        /* ASIDPNE reads as zero on this SMMU (seen on the phone: 0xa7 back);
         * Linux's qcom_iommu sets it the same way and never reads it back.
         * Translation, faults and the tables must have taken. */
        uint32_t control = read32(g, a), table = read32(g, a + 0x20);
        if ((control & 0xa7) != 0xa7 || table != (uintptr_t)root) return fail(g, ADRENO_MMU);
    }
    return true;
}

/* The same readback banks() checks: ASIDPNE reads as zero. */
static bool banks_kept(struct adreno_device *g) {
    for (unsigned cb = 1; cb <= 2; ++cb)
        if ((read32(g, SMMU + cb * 4096) & 0xa7) != 0xa7 ||
            read32(g, SMMU + cb * 4096 + 0x20) != (uintptr_t)g->memory)
            return false;
    return true;
}

static void word(struct adreno_device *g, uint32_t v) {
    uint32_t *ring = (uint32_t *)(g->memory + ADRENO_RING);
    ring[g->ring_write] = v;
    g->ring_write = (g->ring_write + 1) & (ADRENO_RING_WORDS - 1);
}

static void packet(struct adreno_device *g, unsigned opcode, unsigned count) {
    word(g, 0xc0000000 | ((count - 1) << 16) | (opcode << 8));
}

static void state(struct adreno_device *g, unsigned address, uint32_t value) {
    word(g, address);
    word(g, value);
}

static void fence(struct adreno_device *g) {
    packet(g, 0x46, 1);
    word(g, 7); /* HLSQ_FLUSH */
    packet(g, 0x26, 1);
    word(g, 0); /* WAIT_FOR_IDLE */
    packet(g, 0x46, 3);
    word(g, 4);
    word(g, ADRENO_IOVA + ADRENO_FENCE);
    word(g, ++g->sequence);
    sync(g);
    g->pending = true;
    g->polls = 0;
    g->deadline = now(g) + TIMEOUT_US;
    reg(g, 0x01c5, g->ring_write);
}

int adreno_poll(struct adreno_device *g) {
    if (!g || !g->running) return -1;
    if (!g->pending) return 1;
    if ((read32(g, GPU + 4 * 0x0064) & GPU_ERRORS) || (read32(g, SMMU + 0x1058) & 0xc00001fe) ||
        (read32(g, SMMU + 0x2058) & 0xc00001fe)) {
        fail(g, ADRENO_FAULT);
        return -1;
    }
    sync(g);
    volatile uint32_t *done = (volatile uint32_t *)(g->memory + ADRENO_FENCE);
    if (*done == g->sequence && read32(g, GPU + 4 * 0x01c4) == g->ring_write &&
        !(read32(g, GPU + 4 * 0x0030) & (1u << 31))) {
        sync(g);
        g->pending = false;
        return 1;
    }
    if (now(g) >= g->deadline || ++g->polls >= 1000000) {
        fail(g, ADRENO_TIMEOUT);
        return -1;
    }
    return 0;
}

static bool wait_completion(struct adreno_device *g) {
    int status;
    do {
        status = adreno_poll(g);
    } while (status == 0);
    return status == 1;
}

bool adreno_init(struct adreno_device *g, const struct adreno_io *io, void *arena, const void *pm4,
                 unsigned n, const void *pfp, unsigned m, enum pixel_format format,
                 unsigned stride) {
    if (!g || g->attempted) return false;
    g->attempted = true;
    g->error = ADRENO_ARGUMENT;
    unsigned bytes = format == PIXEL_RGB565 ? 2 : format == PIXEL_RGB888 ? 3 : 4;
    if (!io || !io->read || !io->write || !io->now || !io->barrier || !io->restore || !arena ||
        ((uintptr_t)arena & 4095) || (uintptr_t)arena > UINT32_MAX - ADRENO_ARENA_SIZE ||
        (unsigned)format > PIXEL_ARGB8888 || stride < 720 * bytes || stride > 2880 || (stride & 3))
        return false;
    g->io = *io;
    g->memory = arena;
    g->format = format;
    g->stride = stride;
    g->bytes = bytes;
    if (!adreno_firmware_valid(pm4, n, pfp, m)) {
        g->error = ADRENO_FIRMWARE;
        return false;
    }
    g->error = ADRENO_OK;
    g->pm4 = pm4;
    g->pm4_size = n;
    g->pfp = pfp;
    g->pfp_size = m;
    memset(arena, 0, ADRENO_ARENA_SIZE);
    const uint32_t *shader = adreno_shader_xrgb8888;
    unsigned size = sizeof(adreno_shader_xrgb8888);
    if (format == PIXEL_RGB565) {
        shader = adreno_shader_rgb565;
        size = sizeof(adreno_shader_rgb565);
    }
    if (format == PIXEL_RGB888) {
        shader = adreno_shader_rgb888;
        size = sizeof(adreno_shader_rgb888);
    }
    if (format == PIXEL_ARGB8888) {
        shader = adreno_shader_argb8888;
        size = sizeof(adreno_shader_argb8888);
    }
    memcpy(g->memory + ADRENO_SHADER, shader, size);
    g->shader_words = size / 4;
    _Static_assert(sizeof(adreno_shader_copy) <= 0x400, "copy shader slot");
    memcpy(g->memory + ADRENO_COPY_SHADER, adreno_shader_copy, sizeof(adreno_shader_copy));
    return clocks(g) && iommu(g) && engine(g);
}

/* The engine out of reset: A306 settings, register protection, the ring
 * from its start, both microcode images and ME_INIT, then a fence. */
static bool engine(struct adreno_device *g) {
    reg(g, 0x0018, 0);
    sync(g);
    uint32_t version = read32(g, GPU);
    if (version == UINT32_MAX) return fail(g, ADRENO_ENGINE);
    /* A306 settings, with interrupt status polled by the boot CPU. */
    static const uint32_t setup[][2] = {
        {0x3049, 3},       {0x3034, 10},      {0x3035, 10},         {0x0088, UINT32_MAX},
        {0x0012, 0x10},    {0x0033, 0x10},    {0x0020, 1},          {0x0021, 0xa6ffffff},
        {0x0100, 0x30000}, {0x0050, 0x10fff}, {0x0e82, 1},          {0x0010, 0xaaaaaaaa},
        {0x0080, 1},       {0x0063, 0},       {0x0061, UINT32_MAX}, {0x01c1, 0x0800020c},
        {0x045e, 7},
    };
    for (unsigned i = 0; i < sizeof(setup) / sizeof(*setup); ++i) reg(g, setup[i][0], setup[i][1]);
    static const uint32_t protect[] = {0x63000040, 0x62000080, 0x600000cc, 0x60000108, 0x64000140,
                                       0x66000400, 0x65000700, 0x610007d8, 0x620007e0, 0x61001178,
                                       0x64001180, 0x60003300, 0x6b00c000};
    for (unsigned i = 0; i < sizeof(protect) / sizeof(*protect); ++i)
        reg(g, 0x0460 + i, protect[i]);
    g->ring_write = 0;
    g->pending = false;
    reg(g, 0x01c0, ADRENO_IOVA + ADRENO_RING);
    reg(g, 0x01fc, 0x0a000000);
    reg(g, 0x01f8, 0);
    const unsigned char *code = g->pm4;
    for (unsigned i = 4; i < g->pm4_size; i += 4)
        reg(g, 0x01fa,
            (uint32_t)code[i] | (uint32_t)code[i + 1] << 8 | (uint32_t)code[i + 2] << 16 |
                (uint32_t)code[i + 3] << 24);
    reg(g, 0x01c9, 0);
    code = g->pfp;
    for (unsigned i = 4; i < g->pfp_size; i += 4)
        reg(g, 0x01ca,
            (uint32_t)code[i] | (uint32_t)code[i + 1] << 8 | (uint32_t)code[i + 2] << 16 |
                (uint32_t)code[i + 3] << 24);
    reg(g, 0x01d5, 0x000e0602);
    reg(g, 0x01f6, 0);
    static const uint32_t me[] = {0x3f7, 0,     0, 0, 0x80, 0x100, 0x180, 0x6600, 0x150,
                                  0x14e, 0x154, 1, 0, 0,    0,     0,     0};
    packet(g, 0x48, 17);
    for (unsigned i = 0; i < 17; ++i) word(g, me[i]);
    g->running = true;
    fence(g);
    return wait_completion(g);
}

/* KGSL's slumber (kgsl_pwrctrl.c kgsl_pwrctrl_disable, gdsc.c gdsc_disable):
 * an idle GPU's clocks off in the vendor's order (bimc_gpu, bimc_gfx, gmem,
 * ahb, gfx3d), then its power domain collapsed. Its registers, microcode and
 * caches go; the arena in DDR and the SMMU's context banks, outside the
 * domain, stay. A domain that does not report off gets its power and clocks
 * back and the GPU runs on. */
bool adreno_power_off(struct adreno_device *g) {
    if (!g || !g->powered || !g->running || g->pending || g->quarantined) return false;
    /* Idle as adreno_hw_isidle has it for A3xx: the ring drained,
     * RBBM_STATUS's busy bit clear, no error pending. */
    if (read32(g, GPU + 4 * 0x01c4) != g->ring_write ||
        (read32(g, GPU + 4 * 0x0030) & (1u << 31)) || (read32(g, GPU + 4 * 0x0064) & GPU_ERRORS))
        return false;
    sync(g);
    /* From the first clock off, nothing may touch the GPU: not even a
     * panic's adreno_disable. */
    g->powered = false;
    g->clocked = false;
    g->running = false;
    for (unsigned i = 0; i < 5; ++i) {
        bits(g, GCC + branches[i], 1, 0);
        (void)wait_bits(g, GCC + branches[i], 1u << 31, 1u << 31);
    }
    bits(g, GCC + 0x5901c, 1, 1);
    if (!wait_bits(g, GCC + 0x5901c, 1u << 31, 0)) {
        bits(g, GCC + 0x5901c, 1, 0);
        bool back = wait_bits(g, GCC + 0x5901c, 1u << 31, 1u << 31);
        uint64_t settle = now(g);
        for (unsigned i = 0; i < 1000 && now(g) - settle < 2; ++i) { }
        for (unsigned i = 5; i-- > 0;) back &= clock_branch(g, branches[i]);
        if (back) {
            g->powered = true;
            g->clocked = true;
            g->running = true;
        } else {
            g->quarantined = true;
            if (g->error == ADRENO_OK) g->error = ADRENO_POWER;
        }
        return false;
    }
    g->collapsed = true;
    return true;
}

/* Back from adreno_power_off, as KGSL's first command after slumber
 * (adreno.c adreno_start): power and clocks, the context banks again if
 * they lost their state, then the engine out of reset. */
bool adreno_power_on(struct adreno_device *g) {
    if (!g || !g->collapsed || g->quarantined) return false;
    g->collapsed = false;
    if (!clocks(g)) return false;
    if (!banks_kept(g) && !banks(g)) return false;
    return engine(g);
}

static void reserve(struct adreno_device *g, unsigned words) {
    /* Wrap between jobs with legal
     * one-word type-2 NOPs; no PM4 packet ever straddles the physical ring end.
     * The preceding job has retired, so this cannot overwrite live commands. */
    if (g->ring_write > ADRENO_RING_WORDS - words)
        while (g->ring_write) word(g, 0x80000000);
}

static void dispatch(struct adreno_device *g, unsigned x, unsigned y, unsigned width,
                     unsigned height, unsigned shader, unsigned words,
                     const uint32_t constants[8]) {
    unsigned instructions = words / 8;
    packet(g, 0x26, 1);
    word(g, 0);
    state(g, 0x0ee0, 0);
    state(g, 0x0f03, 2);
    packet(g, 0x21, 3);
    word(g, 0x0010);
    word(g, 0xfffcffff);
    word(g, 0x10000);
    state(g, 0x20c0, 0x380);
    state(g, 0x20c1, 0);
    word(g, 0x0008220a); /* NDRANGE and local/group constants */
    word(g, 2 | (16 << 2) | (8 << 12) | (1 << 22));
    word(g, width);
    word(g, x);
    word(g, height);
    word(g, y);
    word(g, 1);
    word(g, 0);
    word(g, 0x1200c);
    word(g, 0xf000);
    word(g, 0x00032214);
    word(g, 0x3006);
    word(g, width / 16);
    word(g, height / 8);
    word(g, 1);
    state(g, 0x221a, 9);
    for (unsigned i = 0; i < 4; ++i) {
        word(g, 0x00030000 | (0x20c4 + i * 4));
        word(g, 0);
        word(g, 0x11);
        word(g, 0);
        word(g, 0);
        state(g, 0x22f4 + i, 0x11);
    }
    state(g, 0x22e4, 0x10400010);
    state(g, 0x22e5, ADRENO_IOVA + ADRENO_PRIVATE);
    state(g, 0x22e6, 0x102);
    state(g, 0x2342, 0);
    state(g, 0x2343, 0);
    state(g, 0x2200, 0x96000100);
    state(g, 0x2201, 0);
    state(g, 0x2205, 7 | (instructions << 24));
    state(g, 0x2206, 0);
    state(g, 0x2207, 0);
    state(g, 0x22c0, 1u << 20);
    state(g, 0x22ff, instructions);
    state(g, 0x22e0, 6 | (4u << 10) | (1u << 23) | (instructions << 24));
    state(g, 0x22e1, 7 | (6u << 10) | (3u << 20));
    state(g, 0x22e2, 0);
    state(g, 0x22e3, ADRENO_IOVA + shader);
    state(g, 0x22ec, 0);
    packet(g, 0x30, 2);
    word(g, (4u << 16) | (6u << 19) | (instructions << 22));
    word(g, ADRENO_IOVA + shader);
    state(g, 0x0e44, 0);
    state(g, 0x0ea0, 0);
    state(g, 0x0ea1, 0x90000000);
    /* Constants start at c5.x (20 scalars); LOAD_STATE constant units are
     * pairs of scalars, while HLSQ/SP const lengths are vec4 counts. */
    packet(g, 0x30, 10);
    word(g, 10 | (6u << 19) | (4u << 22));
    word(g, 1);
    for (unsigned i = 0; i < 8; ++i) word(g, constants[i]);
    state(g, 0x0e23, ADRENO_IOVA + ADRENO_SOURCE);
    state(g, 0x0e22, ADRENO_SOURCE_SIZE);
    state(g, 0x0e23, ADRENO_IOVA + ADRENO_OUTPUT);
    state(g, 0x0e22, g->stride * 1280);
    state(g, 0x0e23, ADRENO_IOVA + ADRENO_UPLOAD);
    state(g, 0x0e22, ADRENO_UPLOAD_SIZE);
    packet(g, 0x31, 1);
    word(g, 0); /* RUN_OPENCL: this fixed kernel only */
    packet(g, 0x26, 1);
    word(g, 0);
    packet(g, 0x21, 3);
    word(g, 0x0010);
    word(g, 0xfffcffff);
    word(g, 0);
}

static bool rect_valid(struct adreno_rect r, unsigned width, unsigned height, bool aligned_origin) {
    return r.width && r.height && r.x < width && r.y < height && r.width <= width - r.x &&
           r.height <= height - r.y && !(r.width & 15) && !(r.height & 7) &&
           (!aligned_origin || (!(r.x & 15) && !(r.y & 7)));
}

static bool ready(struct adreno_device *g) {
    return g && g->running && !g->pending && g->sequence != UINT32_MAX;
}

static void scale(struct adreno_device *g, struct adreno_rect r) {
    const uint32_t constants[] = {ADRENO_IOVA + ADRENO_SOURCE,
                                  ADRENO_IOVA + ADRENO_OUTPUT,
                                  g->stride,
                                  g->bytes,
                                  21846,
                                  0xf800,
                                  0x7e0,
                                  0xff000000};
    dispatch(g, r.x, r.y, r.width, r.height, ADRENO_SHADER, g->shader_words, constants);
    g->x = r.x;
    g->y = r.y;
    g->width = r.width;
    g->height = r.height;
}

bool adreno_submit(struct adreno_device *g, unsigned x, unsigned y, unsigned width,
                   unsigned height) {
    struct adreno_rect r = {x, y, width, height};
    if (!ready(g) || !rect_valid(r, 720, 1280, true)) return false;
    reserve(g, 512);
    scale(g, r);
    fence(g);
    return true;
}

static void checkpoint(struct adreno_device *g, unsigned stage) {
    /* Flush shader writes before a dependent dispatch reads them. A3xx's
     * WAIT_REG_MEM has five payload words (32-bit GPU address), as in KGSL
     * adreno_add_read_cmds. Poll a separate word, never the final job fence.
     * The CPU's bounded job timeout still recovers a hung command processor. */
    packet(g, 0x46, 1);
    word(g, 7);
    packet(g, 0x26, 1);
    word(g, 0);
    packet(g, 0x46, 3);
    word(g, 4);
    word(g, ADRENO_IOVA + ADRENO_FENCE + 4);
    word(g, stage);
    packet(g, 0x3c, 5);
    word(g, 0x13);
    word(g, ADRENO_IOVA + ADRENO_FENCE + 4);
    word(g, stage);
    word(g, UINT32_MAX);
    word(g, 16);
    packet(g, 0x13, 1);
    word(g, 0); /* WAIT_FOR_ME; next dispatch invalidates UCHE */
}

static void copy(struct adreno_device *g, unsigned source, unsigned destination, unsigned w,
                 unsigned h) {
    const uint32_t constants[] = {
        ADRENO_IOVA + source, ADRENO_IOVA + destination, 480 * 4, 4, 0, 0, 0, 0};
    dispatch(g, 0, 0, w, h, ADRENO_COPY_SHADER, sizeof(adreno_shader_copy) / 4, constants);
}

bool adreno_frame(struct adreno_device *g, const struct adreno_copy *c,
                  const struct adreno_rect *patches, unsigned count, struct adreno_rect output) {
    if (!ready(g) || count > ADRENO_PATCHES || (count && !patches) ||
        !rect_valid(output, 720, 1280, true))
        return false;
    if (c &&
        (!rect_valid((struct adreno_rect){c->sx, c->sy, c->width, c->height}, 480, 800, false) ||
         !rect_valid((struct adreno_rect){c->dx, c->dy, c->width, c->height}, 480, 800, false)))
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (!rect_valid(patches[i], 480, 800, true)) return false;
    /* At most 19 dispatches, each <192 words including the dependency fence.
     * Validate the entire request before touching commands or completion RAM. */
    reserve(g, 4096);
    unsigned start = g->ring_write, stage = 0;
    *(uint32_t *)(g->memory + ADRENO_FENCE + 4) = 0;
    if (c) {
        copy(g, ADRENO_SOURCE + (c->sy * 480 + c->sx) * 4, ADRENO_OUTPUT, c->width, c->height);
        checkpoint(g, ++stage);
        copy(g, ADRENO_OUTPUT, ADRENO_SOURCE + (c->dy * 480 + c->dx) * 4, c->width, c->height);
        checkpoint(g, ++stage);
    }
    for (unsigned i = 0; i < count; ++i) {
        struct adreno_rect r = patches[i];
        unsigned offset = (r.y * 480 + r.x) * 4;
        copy(g, ADRENO_UPLOAD + offset, ADRENO_SOURCE + offset, r.width, r.height);
        checkpoint(g, ++stage);
    }
    scale(g, output);
    if (((g->ring_write - start) & (ADRENO_RING_WORDS - 1)) + 8 > 4096)
        return fail(g, ADRENO_ENGINE);
    fence(g);
    return true;
}

static uint32_t pattern(unsigned x, unsigned y) {
    return ((x * 47 + y * 19) & 255) | (((x * 17 + y * 31) & 255) << 8) |
           (((x * 13 + y * 7) & 255) << 16);
}

bool adreno_selftest(struct adreno_device *g) {
    if (!g || !g->running || g->pending) return false;
    uint32_t *source = (uint32_t *)(g->memory + ADRENO_SOURCE);
    /* Vary every channel at every coordinate, including integer scaling
     * boundaries and the last pixel; detect orientation, pitch and packing. */
    for (unsigned y = 0; y < 800; ++y)
        for (unsigned x = 0; x < 480; ++x) source[y * 480 + x] = pattern(x, y);
    memset(g->memory + ADRENO_OUTPUT, 0xa5, ADRENO_OUTPUT_MAX);
    if (!adreno_submit(g, 0, 0, 720, 1280) || !wait_completion(g)) return false;
    for (unsigned y = 0; y < 1280; ++y) {
        const unsigned char *out = g->memory + ADRENO_OUTPUT + y * g->stride;
        for (unsigned x = 0; x < 720; ++x) {
            uint32_t color = source[(y * 5 / 8) * 480 + x * 2 / 3];
            if (g->format == PIXEL_RGB565)
                color = ((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 31);
            if (g->format == PIXEL_ARGB8888) color |= 0xff000000;
            for (unsigned b = 0; b < g->bytes; ++b)
                if (out[x * g->bytes + b] != (unsigned char)(color >> (8 * b)))
                    return fail(g, ADRENO_READBACK);
        }
        for (unsigned x = 720 * g->bytes; x < g->stride; ++x)
            if (out[x] != 0xa5) return fail(g, ADRENO_READBACK);
    }
    /* Exercise a second dispatch with nonzero origins and unchanged borders.
     * This also proves that stale completion words are not accepted. */
    for (unsigned i = 0; i < 480 * 800; ++i) source[i] ^= 0xffffff;
    if (!adreno_submit(g, 16, 8, 16, 8) || !wait_completion(g)) return false;
    for (unsigned y = 0; y < 1280; ++y)
        for (unsigned x = 0; x < 720; ++x) {
            uint32_t color = source[(y * 5 / 8) * 480 + x * 2 / 3];
            if (x < 16 || x >= 32 || y < 8 || y >= 16) color ^= 0xffffff;
            if (g->format == PIXEL_RGB565)
                color = ((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 31);
            if (g->format == PIXEL_ARGB8888) color |= 0xff000000;
            for (unsigned b = 0; b < g->bytes; ++b)
                if (g->memory[ADRENO_OUTPUT + y * g->stride + x * g->bytes + b] !=
                    (unsigned char)(color >> (8 * b)))
                    return fail(g, ADRENO_READBACK);
        }
    /* Prove copy -> overlapping copy -> patch -> scale dependencies on actual
     * hardware before enabling retention. Nonzero, unaligned origins exercise
     * both overlap directions. All untouched retained pixels must survive. */
    struct adreno_rect patch = {16, 32, 16, 8}, out = {0, 0, 128, 96};
    uint32_t *upload = (uint32_t *)(g->memory + ADRENO_UPLOAD);
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned y = 0; y < 800; ++y)
            for (unsigned x = 0; x < 480; ++x) source[y * 480 + x] = pattern(x, y);
        for (unsigned y = 32; y < 40; ++y)
            for (unsigned x = 16; x < 32; ++x) upload[y * 480 + x] = pattern(x, y) ^ 0xabcdef;
        struct adreno_copy c = pass ? (struct adreno_copy){11, 23, 7, 19, 64, 32}
                                    : (struct adreno_copy){7, 19, 11, 23, 64, 32};
        if (!adreno_frame(g, &c, &patch, 1, out) || !wait_completion(g)) return false;
        for (unsigned y = 0; y < 800; ++y)
            for (unsigned x = 0; x < 480; ++x) {
                uint32_t expected = pattern(x, y);
                if (x >= c.dx && x < c.dx + c.width && y >= c.dy && y < c.dy + c.height)
                    expected = pattern(x - c.dx + c.sx, y - c.dy + c.sy);
                if (x >= 16 && x < 32 && y >= 32 && y < 40) expected = pattern(x, y) ^ 0xabcdef;
                if (source[y * 480 + x] != expected) return fail(g, ADRENO_READBACK);
            }
        for (unsigned y = 0; y < out.height; ++y)
            for (unsigned x = 0; x < out.width; ++x) {
                uint32_t color = source[(y * 5 / 8) * 480 + x * 2 / 3];
                if (g->format == PIXEL_RGB565)
                    color = ((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 31);
                if (g->format == PIXEL_ARGB8888) color |= 0xff000000;
                for (unsigned b = 0; b < g->bytes; ++b)
                    if (g->memory[ADRENO_OUTPUT + y * g->stride + x * g->bytes + b] !=
                        (unsigned char)(color >> (8 * b)))
                        return fail(g, ADRENO_READBACK);
            }
    }
    return true;
}
