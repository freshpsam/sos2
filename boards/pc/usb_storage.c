#include "emmc_store.h"
#include "kernel_io.h"
#include "platform.h"
#include "pc.h"

/* The stick SOS2 started from, as the eMMC store's card
 * (include/emmc_store.h, kernel/protection/emmc_store.c): USB mass storage,
 * bulk only, with SCSI's block commands, 512-byte blocks. Of the sticks
 * present, it takes the one whose GPT disk GUID the UEFI loader passed
 * (include/boot/sos2_boot_info.h), whose sos2-system and sos2-data
 * partitions hold the installed system (include/system_store.h); two sticks
 * with that GUID (copies of one image) leave both unused. Transfers go
 * through aligned buffers of its own; a command that fails has its sense
 * read and is tried again after a reset recovery, and a stick that still
 * fails, or goes, stops for the boot.
 * https://www.usb.org/document-library/mass-storage-bulk-only-10 (BOT 1.0:
 * 5, the command and status wrappers; 5.3.4, reset recovery)
 * https://www.t10.org (SBC-3: READ CAPACITY (10), READ (10), WRITE (10),
 * SYNCHRONIZE CACHE (10); SPC-4: REQUEST SENSE's data) */
#define STICKS     4
#define CHUNK      128 /* sectors a command: 64 KiB, one transfer */
#define TIMEOUT_US 5000000
#define FLUSH_US   20000000

struct stick {
    struct usb_endpoint in, out;
    uint8_t slot, interface;
    bool used;
};

static struct stick sticks[STICKS];
static struct stick *card;
/* write_through: SYNCHRONIZE CACHE refused as unknown, so never sent. */
static bool failed, write_through;
static uint32_t tag;
static uint8_t sense_key, sense_code; /* the last REQUEST SENSE's key and additional code */
static uint8_t wrapper[64] __attribute__((aligned(64)));
static uint8_t sense_data[64] __attribute__((aligned(64)));
static uint8_t transfer_buffer[CHUNK * 512] __attribute__((aligned(CHUNK * 512)));

bool usb_storage_attach(unsigned slot, unsigned interface, const struct usb_endpoint *in,
                        const struct usb_endpoint *out) {
    for (unsigned i = 0; i < STICKS; ++i)
        if (!sticks[i].used) {
            sticks[i] = (struct stick){*in, *out, (uint8_t)slot, (uint8_t)interface, true};
            return true;
        }
    return false;
}

void usb_storage_detach(unsigned slot) {
    for (unsigned i = 0; i < STICKS; ++i)
        if (sticks[i].used && sticks[i].slot == slot) {
            sticks[i].used = false;
            if (card == &sticks[i]) failed = true;
        }
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* Bulk-only mass storage reset, then both endpoints' halts cleared, on the
 * device and the controller (5.3.4). */
static void reset_recovery(const struct stick *s) {
    (void)usb_control(s->slot, 0x21, 0xff, 0, s->interface, 0, 0, 500000);
    (void)usb_clear_halt(&s->in);
    (void)usb_clear_halt(&s->out);
}

/* One command: its wrapper, the data in or out of data, and the status
 * wrapper, which a command without data (SYNCHRONIZE CACHE) sends once it
 * is done, so it waits as long as the command may; true when the device
 * reports it passed. */
static bool command(const struct stick *s, const uint8_t *cdb, unsigned cdb_length, void *data,
                    uint32_t length, bool in, uint32_t timeout_us) {
    memset(wrapper, 0, 31);
    put32(wrapper, 0x43425355); /* "USBC" */
    put32(wrapper + 4, ++tag);
    put32(wrapper + 8, length);
    wrapper[12] = in ? 0x80 : 0;
    wrapper[14] = (uint8_t)cdb_length;
    memcpy(wrapper + 15, cdb, cdb_length);
    uint32_t done = 0;
    if (!usb_bulk(&s->out, wrapper, 31, &done, TIMEOUT_US) || done != 31) {
        reset_recovery(s);
        return false;
    }
    if (length && !usb_bulk(in ? &s->in : &s->out, data, length, &done, timeout_us))
        (void)usb_clear_halt(in ? &s->in : &s->out);
    if (!usb_bulk(&s->in, wrapper, 13, &done, timeout_us)) {
        (void)usb_clear_halt(&s->in);
        if (!usb_bulk(&s->in, wrapper, 13, &done, timeout_us)) {
            reset_recovery(s);
            return false;
        }
    }
    if (done != 13 || get32(wrapper) != 0x53425355 || get32(wrapper + 4) != tag ||
        wrapper[12] > 1) {
        reset_recovery(s);
        return false;
    }
    return !wrapper[12] && !get32(wrapper + 8);
}

/* A command tried again, after REQUEST SENSE clears what the device
 * reported (a reset's UNIT ATTENTION, a stick not ready yet), into a buffer
 * of its own: transfer_buffer still holds what a WRITE sends again. One the
 * device refuses (ILLEGAL REQUEST) is not tried again. */
static bool scsi(const struct stick *s, const uint8_t *cdb, unsigned cdb_length, uint32_t length,
                 bool in, uint32_t timeout_us) {
    sense_key = sense_code = 0;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        if (command(s, cdb, cdb_length, transfer_buffer, length, in, timeout_us)) return true;
        static const uint8_t request[6] = {0x03, 0, 0, 0, 18, 0};
        memset(sense_data, 0, 18);
        (void)command(s, request, sizeof(request), sense_data, 18, true, TIMEOUT_US);
        unsigned format = sense_data[0] & 0x7f; /* fixed (70h, 71h) or descriptor (72h, 73h) */
        if (format == 0x70 || format == 0x71) {
            sense_key = sense_data[2] & 15;
            sense_code = sense_data[12];
        } else if (format == 0x72 || format == 0x73) {
            sense_key = sense_data[1] & 15;
            sense_code = sense_data[2];
        }
        if (sense_key == 5) return false;
        kernel_io_wait();
    }
    return false;
}

static bool transfer(const struct stick *s, uint8_t opcode, uint64_t sector, unsigned count,
                     bool in) {
    if (sector > UINT32_MAX || count > CHUNK) return false;
    uint8_t cdb[10] = {opcode,
                       0,
                       (uint8_t)(sector >> 24),
                       (uint8_t)(sector >> 16),
                       (uint8_t)(sector >> 8),
                       (uint8_t)sector,
                       0,
                       (uint8_t)(count >> 8),
                       (uint8_t)count,
                       0};
    return scsi(s, cdb, sizeof(cdb), count * 512, in, TIMEOUT_US);
}

static bool serving(void) { return card && !failed; }

static bool stop(void) {
    failed = true;
    return false;
}

static bool read_many(uint64_t sector, void *data, unsigned count) {
    unsigned char *out = data;
    for (unsigned n; count; count -= n, sector += n, out += (size_t)n * 512) {
        n = count < CHUNK ? count : CHUNK;
        if (!serving() || !transfer(card, 0x28, sector, n, true)) return stop();
        memcpy(out, transfer_buffer, (size_t)n * 512);
    }
    return true;
}

static bool read(uint64_t sector, void *data) { return read_many(sector, data, 1); }

static bool write_many(uint64_t sector, const void *data, unsigned count) {
    if (!serving() || count > 8) return stop();
    memcpy(transfer_buffer, data, (size_t)count * 512);
    return transfer(card, 0x2a, sector, count, false) || stop();
}

static bool write(uint64_t sector, const void *data) { return write_many(sector, data, 1); }

/* SYNCHRONIZE CACHE, which SBC-3 leaves optional: a stick that refuses it
 * as unknown (ILLEGAL REQUEST, an invalid command or field, as Linux's sd
 * takes it) writes through, and is not asked again. */
static bool flush(void) {
    static const uint8_t cdb[10] = {0x35};
    if (!serving()) return stop();
    if (write_through || scsi(card, cdb, sizeof(cdb), 0, false, FLUSH_US)) return true;
    if (sense_key == 5 && (sense_code == 0x20 || sense_code == 0x24)) {
        write_through = true;
        return true;
    }
    return stop();
}

/* The stick whose GPT disk GUID is the one SOS2 came from, ready, its size
 * read; USB started first if it was not yet. */
static bool adopt(const struct platform_devices *d, uint64_t *sectors) {
    if (card) {
        if (failed) return false;
        *sectors = 0;
    } else {
        if (!d->has_boot_disk || !xhci_start()) return false;
        for (unsigned i = 0; i < STICKS; ++i) {
            struct stick *s = &sticks[i];
            static const uint8_t ready[6] = {0}, capacity[10] = {0x25};
            if (!s->used || !scsi(s, ready, sizeof(ready), 0, false, TIMEOUT_US) ||
                !scsi(s, capacity, sizeof(capacity), 8, true, TIMEOUT_US) ||
                be32(transfer_buffer + 4) != 512 || !transfer(s, 0x28, 1, 1, true) ||
                memcmp(transfer_buffer, "EFI PART", 8) ||
                memcmp(transfer_buffer + 56, d->boot_disk_guid, 16))
                continue;
            if (card) { /* a second one: which SOS2 came from is unknown */
                card = 0;
                return false;
            }
            card = s;
        }
        if (!card) return false;
    }
    static const uint8_t capacity[10] = {0x25};
    if (!scsi(card, capacity, sizeof(capacity), 8, true, TIMEOUT_US)) return stop();
    *sectors = (uint64_t)be32(transfer_buffer) + 1;
    return true;
}

const struct emmc_card emmc_card = {adopt,      serving, read, write, read_many,
                                    write_many, flush,   0,    false};
