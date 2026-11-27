#include "pc.h"
#include "platform.h"

/* PCI on a PC: configuration space through ECAM (ACPI's MCFG, read by
 * arch/x64/machine.c), the functions on the first bus and behind its
 * bridges, and their memory BARs as the firmware assigned them, sized with
 * decoding off. The buses found are mapped as one window, with the BARs the
 * drivers ask for. No interrupts: the drivers poll.
 * https://pcisig.com (PCI Local Bus 3.0, 6.1 and 6.2.5; PCIe 7.2.2) */
static struct pci_function functions[PCI_FUNCTIONS];
static unsigned function_count;
static uint64_t ecam;
static uint8_t first_bus, last_bus;
static bool bus_seen[256];

static uintptr_t config(const struct pci_function *f, unsigned offset) {
    return (uintptr_t)(ecam + ((uint64_t)(f->bus - first_bus) << 20 | (uint64_t)f->device << 15 |
                               (uint64_t)f->function << 12 | offset));
}

uint32_t pci_read32(const struct pci_function *f, unsigned offset) {
    return mmio_read32(config(f, offset));
}

void pci_write32(const struct pci_function *f, unsigned offset, uint32_t value) {
    mmio_write32(config(f, offset), value);
}

static uint16_t read16(const struct pci_function *f, unsigned offset) {
    return mmio_read16(config(f, offset));
}

static void write16(const struct pci_function *f, unsigned offset, uint16_t value) {
    mmio_write16(config(f, offset), value);
}

/* The memory BARs: each written with ones, decoding off, for its size. */
static void read_bars(struct pci_function *f) {
    uint16_t command = read16(f, 4);
    write16(f, 4, command & ~3u);
    for (unsigned i = 0; i < 6; ++i) {
        unsigned offset = 0x10 + 4 * i;
        uint32_t low = pci_read32(f, offset);
        if (low & 1) continue; /* I/O space */
        bool wide = ((low >> 1) & 3) == 2 && i < 5;
        pci_write32(f, offset, 0xffffffff);
        uint64_t mask = pci_read32(f, offset) & ~0xfu;
        pci_write32(f, offset, low);
        uint64_t address = low & ~0xfu;
        if (wide) {
            uint32_t high = pci_read32(f, offset + 4);
            pci_write32(f, offset + 4, 0xffffffff);
            mask |= (uint64_t)pci_read32(f, offset + 4) << 32;
            pci_write32(f, offset + 4, high);
            address |= (uint64_t)high << 32;
        } else {
            mask |= 0xffffffff00000000ull;
        }
        if (mask & 0xfffffff0u && address) {
            f->bar[i] = address;
            f->bar_size[i] = ~mask + 1;
        }
        if (wide) ++i;
    }
    write16(f, 4, command);
}

static void scan_bus(unsigned bus, unsigned depth) {
    if (bus < first_bus || bus > last_bus || bus_seen[bus] || depth > 8) return;
    bus_seen[bus] = true;
    for (unsigned device = 0; device < 32; ++device)
        for (unsigned function = 0; function < 8; ++function) {
            struct pci_function probe = {
                .bus = (uint8_t)bus, .device = (uint8_t)device, .function = (uint8_t)function};
            uint32_t id = pci_read32(&probe, 0);
            if ((id & 0xffff) == 0xffff) {
                if (!function) break;
                continue;
            }
            uint8_t header = mmio_read8(config(&probe, 0xe));
            if ((header & 0x7f) == 1) {
                scan_bus(mmio_read8(config(&probe, 0x19)), depth + 1);
            } else if (!(header & 0x7f) && function_count < PCI_FUNCTIONS) {
                struct pci_function *f = &functions[function_count++];
                *f = probe;
                f->vendor = (uint16_t)id;
                f->id = (uint16_t)(id >> 16);
                f->class = pci_read32(f, 8) >> 8;
                read_bars(f);
            }
            if (!function && !(header & 0x80)) break;
        }
}

static bool map(struct platform_devices *d, uint64_t start, uint64_t size) {
    if (d->mmio_count == sizeof(d->mmio) / sizeof(d->mmio[0])) return false;
    d->mmio[d->mmio_count++] = (struct memory_region){start, size};
    return true;
}

/* Through boot.S's identity map, which holds the ECAM window (below 4 GiB
 * on every PC with one), before the kernel maps the buses found. */
void pci_scan(struct platform_devices *d) {
    function_count = 0;
    if (!d->ecam || d->ecam_last_bus < d->ecam_first_bus ||
        d->ecam > (1ull << 32) - ((uint64_t)(d->ecam_last_bus - d->ecam_first_bus + 1) << 20))
        return;
    ecam = d->ecam;
    first_bus = d->ecam_first_bus;
    last_bus = d->ecam_last_bus;
    scan_bus(first_bus, 0);
    /* The buses found, as one window: one of the kernel's device mappings
     * however many buses there are. */
    unsigned low = 256, high = 0;
    for (unsigned bus = first_bus; bus <= last_bus; ++bus)
        if (bus_seen[bus]) {
            if (bus < low) low = bus;
            high = bus;
        }
    if (low <= high)
        (void)map(d, ecam + ((uint64_t)(low - first_bus) << 20), (uint64_t)(high - low + 1) << 20);
}

const struct pci_function *pci_find(uint32_t class, uint32_t mask, unsigned index) {
    for (unsigned i = 0; i < function_count; ++i)
        if ((functions[i].class & mask) == (class & mask) && !index--) return &functions[i];
    return 0;
}

bool pci_map(struct platform_devices *d, const struct pci_function *f, unsigned bar) {
    return bar < 6 && f->bar_size[bar] && map(d, f->bar[bar], f->bar_size[bar]);
}

void pci_enable(const struct pci_function *f) { write16(f, 4, read16(f, 4) | 0x6); }
