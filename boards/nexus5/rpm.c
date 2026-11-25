#include "nexus5.h"
#include "platform.h"

/* Requests to the RPM, the processor that owns MSM8974's shared rails, as
 * the stock kernel sends them (arch/arm/mach-msm/rpm-smd.c over smd.c's
 * channel "rpm_requests"). Shared memory at 0x0FA00000 lists the channel in
 * its allocation table (item 13); its two half-channel records (item 14 + n)
 * and FIFOs (item 338 + n) lie in the RPM's message RAM, which the table of
 * contents names as their base. The apps side sends on half 0 and the
 * FIFOs' first half, receives on the others, and rings the RPM through
 * APCS's IPC register. The RPM channel's records are words, as the RPM
 * reads them. The bootloader leaves its half closed and the RPM's opening.
 *
 * A request is an SMD packet (a 20-byte header whose first word is the
 * length, then the message): the "req" service header, a message header
 * (an id, the active set, a resource type and id), then key-value pairs.
 * The RPM answers each with an ack carrying the id, and an "err" pair if it
 * refused. Polled, on CPU 0. */
enum {
    SMEM = 0x0fa00000,
    SMEM_SIZE = 0x200000,
    /* After four proc_comm records, 32 version words and the heap's info:
     * an entry per item (allocated, offset, size, auxiliary base). */
    SMEM_TOC = SMEM + 208,
    TOC_ENTRY = 16,
    ITEM_CHANNELS = 13, /* entries of name[20], cid, type, ref_count */
    ITEM_SHARED = 14,   /* + cid */
    ITEM_FIFO = 338,    /* + cid */
    CHANNEL_ENTRY = 32,
    EDGE_APPS_RPM = 15,
    RPM_RAM = 0xfc428000,
    RPM_RAM_SIZE = 0x4000,
    IPC = 0xf9011008, /* APCS: bit 0 interrupts the RPM */
    /* A half-channel's words. */
    H_STATE = 0,
    H_DSR,
    H_CTS,
    H_CD,
    H_RI,
    H_HEAD_FLAG,
    H_TAIL_FLAG,
    H_STATE_FLAG,
    H_BLOCK_READ,
    H_TAIL,
    H_HEAD,
    H_WORDS,
    CLOSED = 0,
    OPENING = 1,
    OPENED = 2,
    CLOSING = 4,
    PACKET_HEADER = 20,
    FULL_RESERVE = 4,
    REQUEST_SERVICE = 0x716572, /* "req" */
    ACTIVE_SET = 0,
    ACK_WORDS = 5, /* service, length after these two words, resource, message length, id */
    WAIT_US = 20000,
};

/* Keys and resource types are four ASCII characters, the first lowest. */
#define FOURCC(s) ((uint32_t)(s)[0] | (uint32_t)(s)[1] << 8 | (uint32_t)(s)[2] << 16 | (uint32_t)(s)[3] << 24)

static struct {
    uintptr_t send, receive;           /* half-channel records */
    uintptr_t send_fifo, receive_fifo; /* each fifo_size bytes */
    uint32_t fifo_size;
    uint32_t next_id;
    bool open, failed;
} rpm;

/* An item's address, inside shared memory or the RPM's message RAM. */
uintptr_t nexus5_smem_item(unsigned id, uint32_t *size) {
    uintptr_t entry = SMEM_TOC + TOC_ENTRY * id;
    if (!mmio_read32(entry)) return 0;
    uint32_t offset = mmio_read32(entry + 4), bytes = mmio_read32(entry + 8);
    uint32_t aux = mmio_read32(entry + 12) & ~3u;
    uintptr_t base = aux ? aux : SMEM;
    uint32_t limit = !aux ? SMEM_SIZE : aux == RPM_RAM ? RPM_RAM_SIZE : 0;
    if (!limit || offset > limit || bytes > limit - offset || offset & 3) return 0;
    *size = bytes;
    return base + offset;
}

/* An item allocated in the global heap, as smem.c's smem_alloc2 allocates
 * it: under TCSR's hardware mutex 3, which the apps processor owns while it
 * reads 1 (its PID) back, the item's table entry then the heap's free
 * offset and what remains. An item already there is taken if its size
 * matches. */
enum {
    MUTEX_SMEM = 0xfd484000 + 3 * 0x80,
    APPS_PID = 1,
    HEAP_INFO = SMEM + 192, /* initialized, free offset, remaining */
    FIRST_ALLOCATED_ITEM = 8,
    ITEMS = 512
};

uintptr_t nexus5_smem_alloc(unsigned id, uint32_t size) {
    size = (size + 7) & ~7u;
    if (id < FIRST_ALLOCATED_ITEM || id >= ITEMS || !size) return 0;
    uint64_t start = time_us();
    for (;;) {
        nexus5_store(MUTEX_SMEM, APPS_PID);
        if (mmio_read32(MUTEX_SMEM) == APPS_PID) break;
        if (time_us() - start >= WAIT_US) return 0;
    }
    uintptr_t entry = SMEM_TOC + TOC_ENTRY * id, address = 0;
    uint32_t free = mmio_read32(HEAP_INFO + 4), remaining = mmio_read32(HEAP_INFO + 8);
    if (mmio_read32(entry)) {
        uint32_t found = 0;
        address = nexus5_smem_item(id, &found);
        if (found != size) address = 0;
    } else if (mmio_read32(HEAP_INFO) == 1 && !(free & 7) && free <= SMEM_SIZE &&
               remaining >= size && remaining <= SMEM_SIZE - free) {
        nexus5_store(entry + 4, free);
        nexus5_store(entry + 8, size);
        nexus5_store(entry + 12, 0);
        __asm__ volatile("dsb" ::: "memory");
        nexus5_store(entry, 1);
        nexus5_store(HEAP_INFO + 4, free + size);
        nexus5_store(HEAP_INFO + 8, remaining - size);
        address = SMEM + free;
    }
    __asm__ volatile("dsb" ::: "memory");
    nexus5_store(MUTEX_SMEM, 0);
    return address;
}

static uint32_t half(uintptr_t record, unsigned word) { return mmio_read32(record + 4 * word); }
static void set_half(uintptr_t record, unsigned word, uint32_t value) {
    nexus5_store(record + 4 * word, value);
}

static void ring(void) {
    __asm__ volatile("dsb" ::: "memory");
    nexus5_store(IPC, 1);
}

/* The flags the RPM raised in its record, seen. */
static void clear_flags(void) {
    set_half(rpm.receive, H_HEAD_FLAG, 0);
    set_half(rpm.receive, H_TAIL_FLAG, 0);
    set_half(rpm.receive, H_STATE_FLAG, 0);
}

static void set_state(uint32_t state) {
    uint32_t signals = state == OPENED;
    set_half(rpm.send, H_DSR, signals);
    set_half(rpm.send, H_CTS, signals);
    set_half(rpm.send, H_CD, signals);
    set_half(rpm.send, H_STATE, state);
    set_half(rpm.send, H_STATE_FLAG, 1);
    ring();
}

static bool find_channel(void) {
    uint32_t size = 0;
    uintptr_t table = nexus5_smem_item(ITEM_CHANNELS, &size);
    if (!table) return false;
    for (uint32_t at = 0; at + CHANNEL_ENTRY <= size; at += CHANNEL_ENTRY) {
        uintptr_t entry = table + at;
        static const char name[] = "rpm_requests";
        bool match = (mmio_read32(entry + 24) & 0xff) == EDGE_APPS_RPM;
        for (unsigned i = 0; match && i < sizeof(name); ++i)
            match = *(volatile const char *)(entry + i) == name[i];
        if (!match) continue;
        uint32_t cid = mmio_read32(entry + 20), shared_size = 0, fifo_size = 0;
        if (cid >= 64) return false;
        uintptr_t shared = nexus5_smem_item(ITEM_SHARED + cid, &shared_size);
        uintptr_t fifo = nexus5_smem_item(ITEM_FIFO + cid, &fifo_size);
        if (!shared || shared_size < 2 * 4 * H_WORDS || !fifo || fifo_size < 256 ||
            fifo_size & (fifo_size - 1))
            return false;
        rpm.send = shared;
        rpm.receive = shared + 4 * H_WORDS;
        rpm.fifo_size = fifo_size / 2;
        rpm.send_fifo = fifo;
        rpm.receive_fifo = fifo + rpm.fifo_size;
        return true;
    }
    return false;
}

/* As smd.c opens a channel: from closed, both FIFOs empty and our half
 * opening; once the RPM's half is open, ours too. */
static bool open_channel(void) {
    if (rpm.open) return true;
    if (rpm.failed || !find_channel()) {
        rpm.failed = true;
        return false;
    }
    uint32_t mine = half(rpm.send, H_STATE);
    if (mine == CLOSED || mine == CLOSING) {
        set_half(rpm.receive, H_TAIL, 0);
        set_half(rpm.send, H_HEAD, 0);
        set_half(rpm.send, H_BLOCK_READ, 0);
        set_state(OPENING);
    } else if (mine != OPENING && mine != OPENED) {
        rpm.failed = true;
        return false;
    }
    for (uint64_t start = time_us(); half(rpm.receive, H_STATE) != OPENED;)
        if (time_us() - start >= WAIT_US) {
            rpm.failed = true;
            return false;
        }
    clear_flags();
    if (half(rpm.send, H_STATE) != OPENED) set_state(OPENED);
    /* Acks an earlier session left unread would answer this one's ids, which
     * start at 1 again. */
    set_half(rpm.receive, H_TAIL, half(rpm.receive, H_HEAD) & (rpm.fifo_size - 1));
    rpm.open = true;
    rpm.next_id = 1;
    return true;
}

static void put_words(uintptr_t fifo, uint32_t *at, const uint32_t *words, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        nexus5_store(fifo + *at, words[i]);
        *at = (*at + 4) & (rpm.fifo_size - 1);
    }
}

static bool send_packet(const uint32_t *words, unsigned count) {
    uint32_t bytes = 4 * count, head = half(rpm.send, H_HEAD), tail = half(rpm.send, H_TAIL);
    uint32_t used = (head - tail) & (rpm.fifo_size - 1);
    if (head & 3 || head >= rpm.fifo_size ||
        used + FULL_RESERVE + PACKET_HEADER + bytes > rpm.fifo_size)
        return false;
    const uint32_t header[PACKET_HEADER / 4] = {bytes};
    put_words(rpm.send_fifo, &head, header, PACKET_HEADER / 4);
    put_words(rpm.send_fifo, &head, words, count);
    __asm__ volatile("dsb" ::: "memory");
    set_half(rpm.send, H_HEAD, head);
    __asm__ volatile("dsb" ::: "memory");
    set_half(rpm.send, H_HEAD_FLAG, 1);
    ring();
    return true;
}

/* The next packet's first words, consumed whole; 0 if none has arrived,
 * else its length in bytes. */
static uint32_t receive_packet(uint32_t *words, unsigned count) {
    uint32_t head = half(rpm.receive, H_HEAD), tail = half(rpm.receive, H_TAIL);
    uint32_t ready = (head - tail) & (rpm.fifo_size - 1);
    if (ready < PACKET_HEADER || tail & 3 || tail >= rpm.fifo_size) return 0;
    uint32_t bytes = mmio_read32(rpm.receive_fifo + tail);
    if (!bytes || bytes & 3 || bytes > ready - PACKET_HEADER) return 0;
    uint32_t at = (tail + PACKET_HEADER) & (rpm.fifo_size - 1);
    for (unsigned i = 0; i < bytes / 4; ++i) {
        uint32_t word = mmio_read32(rpm.receive_fifo + at);
        if (i < count) words[i] = word;
        at = (at + 4) & (rpm.fifo_size - 1);
    }
    __asm__ volatile("dsb" ::: "memory");
    set_half(rpm.receive, H_TAIL, at);
    __asm__ volatile("dsb" ::: "memory");
    set_half(rpm.send, H_TAIL_FLAG, 1);
    ring();
    return bytes;
}

/* One active-set request for a resource, two 4-byte pairs, acknowledged
 * without error. */
static bool request(uint32_t type, uint32_t id, uint32_t key0, uint32_t value0, uint32_t key1,
                    uint32_t value1) {
    if (!open_channel()) return false;
    uint32_t message_id = rpm.next_id++;
    const uint32_t data = 2 * 12;
    const uint32_t message[] = {REQUEST_SERVICE, 20 + data, message_id, ACTIVE_SET, type, id,
                                data, key0, 4, value0, key1, 4, value1};
    if (!send_packet(message, sizeof(message) / 4)) return false;
    for (uint64_t start = time_us(); time_us() - start < WAIT_US;) {
        uint32_t ack[ACK_WORDS] = {0};
        uint32_t bytes = receive_packet(ack, ACK_WORDS);
        if (!bytes) continue;
        clear_flags();
        if (bytes < 4 * ACK_WORDS || ack[4] != message_id) continue;
        /* An ack's length past its first two words is 12 without an error. */
        return ack[1] == 12;
    }
    return false;
}

/* MSM8974's logic rail (PM8841 S2, vdd_dig), at a corner of
 * rpm-regulator-smd.h (1 none, 5 normal, 7 super turbo), for the apps
 * processor while it runs: the krait*_dig supply of the stock device tree,
 * 8841_s2_corner_ao, which the RPM takes as the corner less one. */
bool nexus5_rpm_logic_corner(unsigned corner) {
    if (corner < 1 || corner > 7) return false;
    return request(FOURCC("smpb"), 2, FOURCC("swen"), 1, FOURCC("corn"), corner - 1);
}
