#include "nexus5.h"
#include "emmc_store.h"
#include "ext4.h"
#include "platform.h"
#include "qcom_scm.h"
#include "qcom_smd.h"
#include "wire.h"

/* The audio DSP, LPASS's QDSP6, started as the stock kernel starts it
 * through TrustZone (arch/arm/mach-msm/peripheral-loader.c, pil-q6v5-lpass.c
 * and scm-pas.c of Google's hammerhead kernel): its firmware, the split
 * image Android keeps in /system/vendor/firmware (adsp.mdt, the ELF header
 * and program headers with the hash segment that signs them, and adsp.bNN,
 * each loadable segment n), read from the system partition, which SOS2
 * never writes. TrustZone takes the metadata (PAS init), the region the
 * segments fill (memory setup: they load at fixed addresses, 0x0DC00000 to
 * 0x0F000000, inside the firmware hole), and once they are in place checks
 * them and starts the DSP (auth and reset), after which the region is the
 * DSP's alone. Around those calls the crypto engine's clocks are voted on,
 * as scm-pas.c votes them, and the DSP's bus port to memory is clocked, as
 * pil_lpass_reset_trusted clocks it, for as long as the DSP runs. */
enum {
    REGION = 0x0dc00000,
    REGION_SIZE = 0x01400000,
    MDT_MAX = 8192,
    SEGMENTS = 32,
    PT_LOAD = 1,
    EM_QDSP6 = 164,
    GCC = 0xfc400000,
    CE1_CMD_RCGR = GCC + 0x1050,
    CE1_CFG_RCGR = GCC + 0x1054,
    CE1_CBCR = GCC + 0x1044,
    LPASS_Q6_AXI_CBCR = GCC + 0x11c0,
    BRANCH_VOTE = GCC + 0x1484,
    CE1_VOTES = 7 << 3, /* the crypto engine's AHB, AXI and core clocks */
    RCG_UPDATE = 1,
    RCG_FIELDS = 0x1f | 7 << 8,
    CE1_50M = 23 | 1 << 8, /* GPLL0 / 12, the table's lowest, which scm-pas.c asks */
    CLOCK_ENABLE = 1,
    CLOCK_OFF = 1u << 31,
    CLOCK_US = 10000,
    /* APCS's IPC register: bit 8 rings the DSP's SMD, bit 10 its SMP2P. */
    IPC = 0xf9011008,
    IPC_SMD = 1 << 8,
    IPC_SMP2P = 1 << 10
};

/* Uncached, page-aligned and alone in its pages, as TrustZone may guard it
 * while it reads it. */
static uint8_t metadata[MDT_MAX] __attribute__((aligned(4096)));

static struct disk_partition system_partition;
static struct ext4_volume volume;

static bool read_system(void *context, uint64_t sector, void *data, unsigned count) {
    (void)context;
    return emmc_store_read(&system_partition, sector, data, count);
}

static void crypto_clocks(bool on) {
    uint32_t vote = mmio_read32(BRANCH_VOTE);
    nexus5_store(BRANCH_VOTE, on ? vote | CE1_VOTES : vote & ~(uint32_t)CE1_VOTES);
    if (!on) return;
    (void)nexus5_wait_bits(CE1_CBCR, CLOCK_OFF, false, CLOCK_US);
    if ((mmio_read32(CE1_CFG_RCGR) & RCG_FIELDS) != CE1_50M) {
        nexus5_store(CE1_CFG_RCGR, (mmio_read32(CE1_CFG_RCGR) & ~(uint32_t)RCG_FIELDS) | CE1_50M);
        nexus5_store(CE1_CMD_RCGR, mmio_read32(CE1_CMD_RCGR) | RCG_UPDATE);
        (void)nexus5_wait_bits(CE1_CMD_RCGR, RCG_UPDATE, false, CLOCK_US);
    }
}

static bool open_file(const char *name, struct ext4_file *file) {
    char path[] = "/vendor/firmware/adsp.mdt";
    memcpy(path + sizeof(path) - 4, name, 3);
    return ext4_open(&volume, path, file);
}

/* The program headers: each loadable segment (not the hash segment) inside
 * the region, its file no larger than its memory. */
static const char *check(uint32_t size) {
    const uint8_t *p = metadata;
    if (size < 52 || memcmp(p, "\177ELF\1\1\1", 7) || le16(p + 16) != 2 ||
        le16(p + 18) != EM_QDSP6 || le32(p + 28) != 52 || le16(p + 42) != 32)
        return "the firmware's header is not a QDSP6 image";
    unsigned count = le16(p + 44);
    if (!count || count > SEGMENTS || 52 + count * 32 > size)
        return "the firmware's program headers do not fit";
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *ph = p + 52 + i * 32;
        uint32_t address = le32(ph + 12), bytes = le32(ph + 16), memory = le32(ph + 20),
                 flags = le32(ph + 24);
        if (le32(ph) != PT_LOAD || (flags >> 24 & 7) == 2 || !memory) continue;
        if (bytes > memory || address < REGION || memory > REGION_SIZE ||
            address - REGION > REGION_SIZE - memory)
            return "a firmware segment lies outside the DSP's memory";
    }
    return 0;
}

/* The apps processor's SMP2P item for the DSP (SMEM 427 + its PID, 2), as
 * smp2p.c publishes it at the stock kernel's start: its header (magic,
 * version 1 with no features, local PID 0 and remote 2, 16 entries, none
 * valid), the version written last, then the DSP rung. The DSP's firmware
 * negotiates with it while it starts and serves no channel of the apps
 * processor's until it has, so it must exist first. */
enum { SMP2P_APPS_TO_DSP = 429, SMP2P_BYTES = 344, SMP2P_MAGIC = 0x504d5324 };

static bool smp2p(void) {
    uintptr_t item = nexus5_smem_alloc(SMP2P_APPS_TO_DSP, SMP2P_BYTES);
    if (!item) return false;
    for (unsigned i = 0; i < SMP2P_BYTES; i += 4) nexus5_store(item + i, 0);
    nexus5_store(item, SMP2P_MAGIC);
    nexus5_store(item + 8, 2u << 16);
    nexus5_store(item + 12, 16);
    __asm__ volatile("dsb" ::: "memory");
    nexus5_store(item + 4, 1);
    __asm__ volatile("dsb" ::: "memory");
    nexus5_store(IPC, IPC_SMP2P);
    return true;
}

/* The loader's place: the next program header, and how far into its file. */
static unsigned next_segment;
static uint32_t segment_done;
static bool started;

static const char *begin(void) {
    struct ext4_file mdt;
    if (!smp2p()) return "no shared memory for the audio DSP";
    if (!emmc_store_find(nexus5_devices(), "system", &system_partition))
        return "no system partition";
    if (!ext4_mount(&volume, read_system, 0, system_partition.sectors))
        return "the system partition is not ext4";
    if (!open_file("mdt", &mdt)) return "no audio firmware in /system/vendor/firmware";
    if (!mdt.size || mdt.size > MDT_MAX || !ext4_read(&volume, &mdt, 0, metadata, mdt.size))
        return "the firmware's metadata could not be read";
    const char *error = check((uint32_t)mdt.size);
    if (error) return error;
    if (!qcom_scm_init()) return "TrustZone has no peripheral loader";
    crypto_clocks(true);
    bool accepted = qcom_scm_pas_init(QCOM_PAS_ADSP, (uintptr_t)metadata) &&
                    qcom_scm_pas_memory(QCOM_PAS_ADSP, REGION, REGION_SIZE);
    crypto_clocks(false);
    if (!accepted) return "TrustZone refused the audio firmware";
    started = true;
    return 0;
}

/* Up to 1 MiB of the next segment's file, or its zeros once the file is in;
 * true when every segment is in place. */
static bool load(const char **error) {
    const uint8_t *p = metadata;
    for (unsigned count = le16(p + 44); next_segment < count; ++next_segment, segment_done = 0) {
        const uint8_t *ph = p + 52 + next_segment * 32;
        uint32_t address = le32(ph + 12), bytes = le32(ph + 16), memory = le32(ph + 20);
        if (le32(ph) != PT_LOAD || (le32(ph + 24) >> 24 & 7) == 2 || !memory) continue;
        if (segment_done < bytes) {
            char name[] = {'b', (char)('0' + next_segment / 10), (char)('0' + next_segment % 10)};
            struct ext4_file file;
            uint32_t n = bytes - segment_done;
            if (n > (1u << 20)) n = 1u << 20;
            if (!open_file(name, &file) || file.size != bytes) {
                *error = "a firmware segment is missing";
                return false;
            }
            if (!ext4_read(&volume, &file, segment_done,
                           (void *)(uintptr_t)(address + segment_done), n)) {
                *error = "a firmware segment could not be read";
                return false;
            }
            segment_done += n;
            return false;
        }
        memset((void *)(uintptr_t)(address + bytes), 0, memory - bytes);
    }
    return true;
}

bool nexus5_adsp_load_step(const char **error) {
    *error = 0;
    if (!started) {
        *error = begin();
        return false;
    }
    if (!load(error)) {
        if (*error) {
            crypto_clocks(true);
            (void)qcom_scm_pas_stop(QCOM_PAS_ADSP);
            crypto_clocks(false);
        }
        return false;
    }
    nexus5_store(LPASS_Q6_AXI_CBCR, mmio_read32(LPASS_Q6_AXI_CBCR) | CLOCK_ENABLE);
    (void)nexus5_wait_bits(LPASS_Q6_AXI_CBCR, CLOCK_OFF, false, CLOCK_US);
    crypto_clocks(true);
    bool running = qcom_scm_pas_start(QCOM_PAS_ADSP);
    crypto_clocks(false);
    if (!running) {
        nexus5_store(LPASS_Q6_AXI_CBCR, mmio_read32(LPASS_Q6_AXI_CBCR) & ~(uint32_t)CLOCK_ENABLE);
        *error = "TrustZone did not start the audio DSP";
    }
    return true;
}

/* A channel the DSP opened to the apps processor (edge 1) in the shared
 * memory's allocation table: its two half-channel records (item 14 + its
 * number) and its FIFOs (item 338 + its number), which smd.c opens. */
enum {
    ITEM_CHANNELS = 13,
    ITEM_SHARED = 14,
    ITEM_FIFO = 338,
    CHANNEL_ENTRY = 32,
    EDGE_APPS_DSP = 1
};

static void ring(void *context) {
    (void)context;
    __asm__ volatile("dsb" ::: "memory");
    nexus5_store(IPC, IPC_SMD);
}

bool nexus5_adsp_channel(const char *name, struct smd_channel *out, uint64_t now) {
    uint32_t size = 0;
    uintptr_t table = nexus5_smem_item(ITEM_CHANNELS, &size);
    size_t length = strlen(name);
    if (!table || length >= 20) return false;
    for (uint32_t at = 0; at + CHANNEL_ENTRY <= size; at += CHANNEL_ENTRY) {
        uintptr_t entry = table + at;
        bool match = (mmio_read32(entry + 24) & 0xff) == EDGE_APPS_DSP;
        for (unsigned i = 0; match && i <= length; ++i)
            match = *(volatile const char *)(entry + i) == (i < length ? name[i] : 0);
        if (!match) continue;
        uint32_t cid = mmio_read32(entry + 20), info_size = 0, fifo_size = 0;
        if (cid >= 64) return false;
        uintptr_t info = nexus5_smem_item(ITEM_SHARED + cid, &info_size);
        uintptr_t fifo = nexus5_smem_item(ITEM_FIFO + cid, &fifo_size);
        return info && fifo && info_size == 40 &&
               smd_open(out, (volatile void *)info, info_size, (volatile void *)fifo, fifo_size,
                        ring, 0, now);
    }
    return false;
}
