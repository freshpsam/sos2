#include "x86.h"
#include "machine.h"
#include "platform.h"
#include "sos2_boot_info.h"
#include <stddef.h>

/* x64's handoff and machine description for QEMU's microvm. Its PVH start
 * info stands in for a device tree: the memory map, the initrd (QEMU's
 * -initrd, module 0) and the command line, which carries SOS2's sos2.*
 * arguments and any virtio-mmio devices listed there
 * (virtio_mmio.device=SIZE@BASE:IRQ). QEMU lists them only for Linux, so on
 * QEMU (ACPI's BOCHS tables) the kernel looks for microvm's transports at
 * their fixed addresses. ACPI gives the CPUs (MADT), the IOAPICs and the
 * reset register (FADT). Everything is read once, before the
 * kernel's own tables, through boot.S's identity map, and copied: nothing
 * of the handoff stays in use. On a PC, SOS2's UEFI loader hands over its
 * own boot information instead (include/boot/sos2_boot_info.h): the same
 * memory map, resources and command line, ACPI's root pointer, the GOP
 * framebuffer and the disk it came from. The clock is the CMOS RTC, in UTC
 * as QEMU keeps it.
 * https://xenbits.xen.org/docs/unstable/misc/pvh.html
 * https://uefi.org/specs/ACPI/6.5/05_ACPI_Software_Programming_Model.html */
enum {
    START_INFO_MAGIC = 0x336ec578,
    MEMMAP_RAM = 1,
    LOW_MEMORY = 0x100000,
    /* QEMU microvm's virtio-mmio transports: 512 bytes each from here. */
    MICROVM_VIRTIO = 0xfeb00000,
    MICROVM_TRANSPORTS = 24,
    VIRTIO_MAGIC = 0x74726976 /* "virt" */
};

struct start_info {
    uint32_t magic, version, flags, nr_modules;
    uint64_t modlist, cmdline, rsdp, memmap;
    uint32_t memmap_entries, reserved;
};

struct module {
    uint64_t paddr, size, cmdline, reserved;
};

struct memmap_entry {
    uint64_t address, size;
    uint32_t type, reserved;
};

uintptr_t x86_ioapics[X86_IOAPICS];
unsigned x86_ioapic_count;
static struct machine machine;
static uintptr_t lapic = 0xfee00000;

/* The FADT's reset register (a generic address) and its value. */
static struct {
    uint8_t space, width, offset, access;
    uint64_t address;
    uint8_t value;
    bool present;
} reset;

static uint32_t read32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t read64(const unsigned char *p) { return read32(p) | (uint64_t)read32(p + 4) << 32; }

static bool checksum(const unsigned char *p, size_t n) {
    unsigned char sum = 0;
    while (n--) sum += *p++;
    return !sum;
}

/* An address below 4 GiB, where boot.S's map reaches. */
static const unsigned char *low(uint64_t address, uint64_t size) {
    return address && size <= (1ull << 32) && address <= (1ull << 32) - size
               ? (const unsigned char *)(uintptr_t)address
               : 0;
}

static uint64_t number(const char *s, const char **end) {
    uint64_t value = 0;
    unsigned base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    for (;; ++s) {
        unsigned digit = *s >= '0' && *s <= '9'   ? (unsigned)(*s - '0')
                         : *s >= 'a' && *s <= 'f' ? (unsigned)(*s - 'a' + 10)
                         : *s >= 'A' && *s <= 'F' ? (unsigned)(*s - 'A' + 10)
                                                  : 16;
        if (digit >= base) break;
        value = value * base + digit;
    }
    *end = s;
    return value;
}

/* The command line's tokens: SOS2's own arguments, kept whole and in order
 * as kernel/fdt.c keeps a device tree's (a token that does not fit ends
 * the list), and QEMU's virtio-mmio devices. */
static bool command_line(const char *p, struct platform_devices *d) {
    size_t used = 0;
    bool full = false;
    d->bootargs[0] = 0;
    for (size_t i = 0; p[i];) {
        while (p[i] == ' ' || p[i] == '\t') ++i;
        size_t start = i;
        while (p[i] && p[i] != ' ' && p[i] != '\t') ++i;
        size_t n = i - start;
        const char *token = p + start;
        if (n >= 5 && !memcmp(token, "sos2.", 5) && !full) {
            if (used + (used ? 1 : 0) + n >= sizeof(d->bootargs)) {
                full = true;
                continue;
            }
            if (used) d->bootargs[used++] = ' ';
            memcpy(d->bootargs + used, token, n);
            used += n;
            d->bootargs[used] = 0;
        } else if (n > 19 && !memcmp(token, "virtio_mmio.device=", 19)) {
            const char *s = token + 19, *end;
            uint64_t size = number(s, &end);
            if (*end == 'K' || *end == 'k') {
                size <<= 10;
                ++end;
            }
            if (*end != '@' || !size || size > 0x1000) return false;
            uint64_t base = number(end + 1, &end);
            if (!base || base & 0x1ff || base > UINT32_MAX - size) return false;
            if (d->virtio_count == MAX_VIRTIO) return false;
            d->virtio[d->virtio_count++] = (uintptr_t)base;
        }
    }
    return true;
}

/* A table the RSDT or XSDT lists, by signature, checked. */
static const unsigned char *acpi_table(const unsigned char *root, bool extended,
                                       const char *signature) {
    uint32_t length = read32(root + 4);
    unsigned width = extended ? 8 : 4;
    for (uint32_t offset = 36; offset + width <= length; offset += width) {
        uint64_t address = extended ? read64(root + offset) : read32(root + offset);
        const unsigned char *table = low(address, 36);
        if (!table || memcmp(table, signature, 4)) continue;
        uint32_t size = read32(table + 4);
        if (size >= 36 && low(address, size) && checksum(table, size)) return table;
    }
    return 0;
}

static const unsigned char *find_rsdp(uint64_t hint) {
    const unsigned char *p = low(hint, 20);
    if (p && !memcmp(p, "RSD PTR ", 8) && checksum(p, 20)) return p;
    /* The BIOS area, on 16-byte boundaries. */
    for (uintptr_t at = 0xe0000; at < 0x100000; at += 16) {
        p = (const unsigned char *)at;
        if (!memcmp(p, "RSD PTR ", 8) && checksum(p, 20)) return p;
    }
    return 0;
}

/* Whether the ACPI tables are QEMU's own. */
static bool qemu;

static bool acpi(uint64_t hint, struct platform_devices *d) {
    const unsigned char *rsdp = find_rsdp(hint);
    if (!rsdp) return false;
    qemu = !memcmp(rsdp + 9, "BOCHS ", 6);
    bool extended = rsdp[15] >= 2 && read64(rsdp + 24) && checksum(rsdp, 36);
    const unsigned char *root = extended ? low(read64(rsdp + 24), 36) : low(read32(rsdp + 16), 36);
    if (!root || !checksum(root, read32(root + 4))) return false;
    const unsigned char *madt = acpi_table(root, extended, "APIC");
    if (!madt) return false;
    lapic = read32(madt + 36);
    uint32_t length = read32(madt + 4);
    for (uint32_t offset = 44; offset + 2 <= length;) {
        const unsigned char *e = madt + offset;
        unsigned type = e[0], size = e[1];
        if (size < 2 || offset + size > length) return false;
        uint32_t id = UINT32_MAX;
        /* Enabled processors only, not those a hotplug could add. */
        if (type == 0 && size >= 8 && (read32(e + 4) & 1)) id = e[3];
        else if (type == 9 && size >= 16 && (read32(e + 8) & 1)) id = read32(e + 4);
        else if (type == 1 && size >= 12 && x86_ioapic_count < X86_IOAPICS)
            x86_ioapics[x86_ioapic_count++] = read32(e + 4);
        else if (type == 5 && size >= 12 && read64(e + 4) < (1ull << 32))
            lapic = (uintptr_t)read64(e + 4);
        if (id != UINT32_MAX && d->cpu_count < MAX_BOOT_CPUS) {
            for (unsigned i = 0; i < d->cpu_count; ++i)
                if (d->cpu_mpidr[i] == id) return false;
            d->cpu_mpidr[d->cpu_count++] = id;
        }
        offset += size;
    }
    /* PCI Express configuration space: segment 0's first window. */
    const unsigned char *mcfg = acpi_table(root, extended, "MCFG");
    for (uint32_t offset = 44; mcfg && offset + 16 <= read32(mcfg + 4); offset += 16) {
        const unsigned char *e = mcfg + offset;
        if (!e[8] && !e[9] && e[10] <= e[11] && read64(e)) {
            d->ecam = read64(e);
            d->ecam_first_bus = e[10];
            d->ecam_last_bus = e[11];
            break;
        }
    }
    const unsigned char *fadt = acpi_table(root, extended, "FACP");
    /* Fixed hardware in I/O space, unless the machine is hardware-reduced
     * (microvm) or its power button is AML's. */
    if (fadt && read32(fadt + 4) >= 116 && !(read32(fadt + 112) & (1u << 20))) {
        d->smi_command = read32(fadt + 48) <= 0xffff ? (uint16_t)read32(fadt + 48) : 0;
        d->acpi_enable = fadt[52];
        d->pm1a_event = read32(fadt + 56) <= 0xffff ? (uint16_t)read32(fadt + 56) : 0;
        d->pm1b_event = read32(fadt + 60) <= 0xffff ? (uint16_t)read32(fadt + 60) : 0;
        d->pm1a_control = read32(fadt + 64) <= 0xffff ? (uint16_t)read32(fadt + 64) : 0;
        d->pm1_event_length = fadt[88];
        d->fixed_power_button =
            !(read32(fadt + 112) & (1u << 4)) && d->pm1a_event && d->pm1_event_length >= 4;
    }
    if (fadt && read32(fadt + 4) >= 129 && (read32(fadt + 112) & (1u << 10)) && fadt[116] <= 1 &&
        read64(fadt + 120)) {
        reset.space = fadt[116];
        reset.width = fadt[117];
        reset.offset = fadt[118];
        reset.access = fadt[119];
        reset.address = read64(fadt + 120);
        reset.value = fadt[128];
        reset.present = reset.space == 1 ? reset.address <= 0xffff : reset.address < (1ull << 32);
    }
    return d->cpu_count != 0;
}

/* QEMU puts a PVH initrd flush against the top of RAM below 4 GiB, and
 * microvm's firmware (qboot) may reserve pages there for itself and load the
 * initrd over them afterwards (it does with a disk attached): the module list
 * says what they hold. The module's range becomes RAM, out of the firmware's
 * reservations, where the bundle is used in place. */
static bool claim_module(struct platform_devices *d, uint64_t start, uint64_t end) {
    if (start < LOW_MEMORY) return false;
    for (unsigned i = 0; i < d->ram_region_count; ++i) {
        struct memory_region *r = &d->ram_regions[i];
        uint64_t top = r->start + r->size;
        if (r->start > start || start > top || top >= end) continue;
        for (unsigned j = 0; j < d->ram_region_count; ++j)
            if (d->ram_regions[j].start >= top && d->ram_regions[j].start < end) return false;
        r->size = end - r->start;
    }
    for (unsigned i = 1; i < d->reserved_region_count;) {
        struct memory_region *r = &d->reserved_regions[i];
        uint64_t low = r->start, high = r->start + r->size;
        if (high <= start || end <= low) {
            ++i;
            continue;
        }
        if (low < start && end < high) {
            if (d->reserved_region_count == MAX_RESERVED_REGIONS) return false;
            d->reserved_regions[d->reserved_region_count++] =
                (struct memory_region){end, high - end};
        }
        if (low < start) *r = (struct memory_region){low, start - low}, ++i;
        else if (end < high) *r = (struct memory_region){end, high - end}, ++i;
        else *r = d->reserved_regions[--d->reserved_region_count];
    }
    return true;
}

/* The memory map, PVH's or the UEFI loader's (the same entries), with the
 * low megabyte (the BIOS's data, the SMP trampoline's page) kept out of the
 * pool. */
static bool memory_map(const struct memmap_entry *map, uint32_t count, struct platform_devices *d) {
    if (!count) return false;
    d->reserved_regions[d->reserved_region_count++] = (struct memory_region){0, LOW_MEMORY};
    for (uint32_t i = 0; i < count; ++i) {
        const struct memmap_entry *e = &map[i];
        if (!e->size || e->address > UINT64_MAX - e->size) return false;
        struct memory_region r = {e->address, e->size};
        if (e->type == MEMMAP_RAM) {
            if (d->ram_region_count == MAX_RAM_REGIONS) continue;
            d->ram_regions[d->ram_region_count++] = r;
        } else {
            if (d->reserved_region_count == MAX_RESERVED_REGIONS) return false;
            d->reserved_regions[d->reserved_region_count++] = r;
        }
    }
    return d->ram_region_count != 0;
}

static bool module(uint64_t start, uint64_t size, struct platform_devices *d) {
    if (!size || start > UINT64_MAX - size) return false;
    d->initrd_start = start;
    d->initrd_end = start + size;
    d->has_initrd_start = d->has_initrd_end = true;
    return claim_module(d, d->initrd_start, d->initrd_end);
}

_Static_assert(sizeof(struct memmap_entry) == sizeof(struct sos2_memmap_entry),
               "memory map entries differ");

/* A PC's boot information from SOS2's UEFI loader: no virtio devices, and
 * the GOP framebuffer, which must lie below 4 GiB, where boot.S's map
 * reaches until vm_init. */
static bool uefi_boot(const struct sos2_boot_info *b, struct platform_devices *d) {
    if (b->version != SOS2_BOOT_INFO_VERSION || b->size != sizeof(*b) ||
        b->memmap_entries > SOS2_BOOT_MEMMAP_MAX ||
        !memory_map((const struct memmap_entry *)b->memmap, b->memmap_entries, d) ||
        !module(b->module, b->module_size, d))
        return false;
    bool terminated = false;
    for (size_t i = 0; i < sizeof(b->command_line) && !terminated; ++i)
        terminated = !b->command_line[i];
    if (!terminated || !command_line(b->command_line, d) || d->virtio_count || !acpi(b->rsdp, d))
        return false;
    uint64_t bytes = (uint64_t)b->stride * b->height;
    if (b->format != SOS2_PIXEL_XRGB8888 || !b->width || !b->height || b->stride / 4 < b->width ||
        b->stride % 4 || bytes > b->framebuffer_size || !low(b->framebuffer, bytes))
        return false;
    d->framebuffer = (struct framebuffer){(void *)(uintptr_t)b->framebuffer, b->width, b->height,
                                          b->stride, PIXEL_XRGB8888};
    d->framebuffer_size = b->framebuffer_size;
    d->has_framebuffer = true;
    memcpy(d->boot_disk_guid, b->disk_guid, sizeof(d->boot_disk_guid));
    d->has_boot_disk = true;
    return true;
}

bool machine_discover(const void *handoff, struct platform_devices *d) {
    *d = (struct platform_devices){0};
    const struct sos2_boot_info *b = handoff;
    if (low((uintptr_t)b, sizeof(*b)) && b->magic == SOS2_BOOT_INFO_MAGIC) return uefi_boot(b, d);
    const struct start_info *s = handoff;
    if (!low((uintptr_t)s, sizeof(*s)) || s->magic != START_INFO_MAGIC || s->version < 1)
        return false;
    const struct memmap_entry *map =
        (const void *)low(s->memmap, (uint64_t)s->memmap_entries * sizeof(struct memmap_entry));
    if (!map || !memory_map(map, s->memmap_entries, d)) return false;
    if (s->nr_modules) {
        const struct module *m = (const struct module *)low(s->modlist, sizeof(struct module));
        if (!m || !module(m->paddr, m->size, d)) return false;
    }
    const char *cmdline = (const char *)low(s->cmdline, 1);
    if (cmdline && !command_line(cmdline, d)) return false;
    if (!acpi(s->rsdp, d)) return false;
    /* Empty transports still answer with the magic (device ID 0); the
     * drivers look for theirs. */
    if (!d->virtio_count && qemu)
        for (unsigned i = 0; i < MICROVM_TRANSPORTS && d->virtio_count < MAX_VIRTIO; ++i) {
            uintptr_t base = MICROVM_VIRTIO + 512 * i;
            if (mmio_read32(base) == VIRTIO_MAGIC) d->virtio[d->virtio_count++] = base;
        }
    return true;
}

/* The MMIO the kernel maps (arch/x64/mmu.c), in whole pages: the local
 * APIC (xAPIC's registers), the IOAPICs, the virtio transports, an MMIO
 * reset register, a PC's framebuffer, write-combining, and its drivers'
 * PCI windows. */
static bool mapping(uint64_t start, uint64_t size, bool write_combining) {
    uint64_t first = start & ~(uint64_t)4095, end = (start + size + 4095) & ~(uint64_t)4095;
    if (!size || start > UINT64_MAX - size - 4095 || end - first > UINT32_MAX ||
        machine.device_count == sizeof(machine.devices) / sizeof(machine.devices[0]))
        return false;
    machine.devices[machine.device_count++] =
        (struct machine_mapping){(uintptr_t)first, (uint32_t)(end - first), write_combining};
    return true;
}

static bool device(uintptr_t start, uint32_t size) { return mapping(start, size, false); }

bool machine_init(const struct platform_devices *d) {
    machine = (struct machine){0};
    machine.timer = (struct timer_route){.x2apic = x86_features.x2apic,
                                         .lapic = x86_features.x2apic ? 0 : lapic};
    if (!x86_features.x2apic && !device(lapic, 0x1000)) return false;
    for (unsigned i = 0; i < x86_ioapic_count; ++i)
        if (!device(x86_ioapics[i], 0x20)) return false;
    for (size_t i = 0; i < d->virtio_count; ++i)
        if (!device(d->virtio[i], 0x200)) return false;
    if (reset.present && !reset.space && !device((uintptr_t)reset.address, 1)) return false;
    if (d->has_framebuffer && !mapping((uintptr_t)d->framebuffer.pixels, d->framebuffer_size, true))
        return false;
    for (size_t i = 0; i < d->mmio_count; ++i)
        if (!mapping(d->mmio[i].start, d->mmio[i].size, false)) return false;
    return true;
}

const struct machine *machine_get(void) { return &machine; }

/* The CMOS clock: wait out an update, read until two reads agree, then
 * decode BCD and the 12-hour form where register B says so. */
static uint8_t cmos(uint8_t index) {
    x86_outb(0x70, index);
    return x86_inb(0x71);
}

enum rtc_kind rtc_kind(void) { return RTC_UNIX; }

bool rtc_read(uint32_t *seconds) {
    if (!seconds) return false;
    uint8_t now[7], again[7];
    for (unsigned attempt = 0;; ++attempt) {
        if (attempt == 8) return false;
        for (unsigned spins = 0; cmos(0x0a) & 0x80; ++spins)
            if (spins > 1000000) return false;
        static const uint8_t index[7] = {0x00, 0x02, 0x04, 0x07, 0x08, 0x09, 0x32};
        for (unsigned i = 0; i < 7; ++i) now[i] = cmos(index[i]);
        for (unsigned spins = 0; cmos(0x0a) & 0x80; ++spins)
            if (spins > 1000000) return false;
        for (unsigned i = 0; i < 7; ++i) again[i] = cmos(index[i]);
        if (!memcmp(now, again, sizeof(now))) break;
    }
    uint8_t b = cmos(0x0b);
    bool pm = !(b & 2) && (now[2] & 0x80);
    now[2] &= 0x7f;
    if (!(b & 4))
        for (unsigned i = 0; i < 7; ++i) now[i] = (uint8_t)((now[i] >> 4) * 10 + (now[i] & 15));
    unsigned hour = now[2] % 12 + (pm ? 12 : 0), year = now[6] * 100u + now[5];
    if (b & 2) hour = now[2];
    if (now[6] < 19 || now[6] > 99) year = 2000 + now[5];
    unsigned month = now[4], day = now[3];
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || now[1] > 59 || now[0] > 59 ||
        year < 1970 || year > 2105)
        return false;
    /* Days since 1970 (Howard Hinnant's days_from_civil). */
    int y = (int)year - (month <= 2);
    unsigned era = (unsigned)(y / 400), yoe = (unsigned)(y - (int)era * 400);
    unsigned shifted = month > 2 ? month - 3 : month + 9; /* March first */
    unsigned doy = (153 * shifted + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint64_t days = (uint64_t)era * 146097 + doe - 719468;
    uint64_t total = days * 86400 + hour * 3600u + now[1] * 60u + now[0];
    if (total > UINT32_MAX) return false;
    *seconds = (uint32_t)total;
    return true;
}

_Noreturn void machine_restart(void) {
    if (reset.present) {
        if (reset.space == 1) x86_outb((uint16_t)reset.address, reset.value);
        else mmio_write8((uintptr_t)reset.address, reset.value);
    }
    for (unsigned spins = 0; spins < 100000 && (x86_inb(0x64) & 2); ++spins) { }
    x86_outb(0x64, 0xfe); /* the keyboard controller's reset line */

    /* A triple fault: no IDT, then an exception. */
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } none = {0, 0};

    __asm__ volatile("lidt %0\n int3" : : "m"(none));
    for (;;) __asm__ volatile("cli\n hlt");
}
