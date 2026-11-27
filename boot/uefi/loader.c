/* SOS2's UEFI loader, EFI/BOOT/BOOTX64.EFI on a PC's stick
 * (tools/package_pc.py). It finds the GPT disk it was started from, reads
 * the system image from that disk's sos2-system partition
 * (include/boot/pc_image.h), loads the kernel's ELF where it is linked and
 * the resources below 4 GiB, keeps a GOP mode with at least 800 lines where
 * the monitor has one, and starts the kernel at its 64-bit entry with the
 * memory map, the resources, the command line, ACPI's root pointer, the
 * framebuffer and the disk's GUID (include/boot/sos2_boot_info.h), after
 * leaving the firmware's boot services. A system it cannot start, such as
 * the cleared one an interrupted update leaves, it names on the screen, then
 * returns to the firmware after a key. */
#include "efi.h"
#include "pc_image.h"
#include "sos2_boot_info.h"

#define SECTOR        512u
#define PAGE          4096u
#define BELOW_4G      0xffffffffull
#define SCRATCH_PAGES 16u /* a GPT header and its entries, at most 64 KiB */
#define READ_CHUNK    (8u << 20)
#define MIN_LINES     800u

static EFI_SYSTEM_TABLE *system_table;
static EFI_BOOT_SERVICES *services;
static const char *problem;
static EFI_STATUS problem_status;

static const EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static const EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static const EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static const EFI_GUID graphics_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static const EFI_GUID acpi20_guid = EFI_ACPI_20_TABLE_GUID;
static const EFI_GUID acpi10_guid = EFI_ACPI_10_TABLE_GUID;
static const EFI_GUID memory_attribute_guid = EFI_MEMORY_ATTRIBUTE_PROTOCOL_GUID;

/* GCC may call these for copies and clears even when freestanding. */
void *memcpy(void *destination, const void *source, size_t size) {
    unsigned char *d = destination;
    const unsigned char *s = source;
    while (size--) *d++ = *s++;
    return destination;
}

void *memset(void *destination, int value, size_t size) {
    unsigned char *d = destination;
    while (size--) *d++ = (unsigned char)value;
    return destination;
}

int memcmp(const void *a, const void *b, size_t size) {
    const unsigned char *x = a, *y = b;
    for (; size; --size, ++x, ++y)
        if (*x != *y) return *x - *y;
    return 0;
}

static void print(const char *text) {
    CHAR16 line[130];
    unsigned n = 0;
    for (;; ++text) {
        if (*text == '\n') line[n++] = '\r';
        if (*text) line[n++] = (unsigned char)*text;
        if (!*text || n >= 126) {
            line[n] = 0;
            system_table->con_out->output_string(system_table->con_out, line);
            n = 0;
        }
        if (!*text) return;
    }
}

static void print_hex(uint64_t value) {
    char text[19] = "0x";
    unsigned digits = 1;
    while (digits < 16 && value >> (4 * digits)) ++digits;
    for (unsigned i = 0; i < digits; ++i)
        text[2 + i] = "0123456789abcdef"[value >> (4 * (digits - 1 - i)) & 15];
    text[2 + digits] = 0;
    print(text);
}

static bool fail(const char *why, EFI_STATUS status) {
    problem = why;
    problem_status = status;
    return false;
}

static bool same_guid(const EFI_GUID *a, const EFI_GUID *b) { return !memcmp(a, b, sizeof(*a)); }

static uint32_t crc32(const void *data, uint64_t size) {
    uint32_t crc = 0;
    services->calculate_crc32(data, size, &crc);
    return crc;
}

static uint64_t pages_for(uint64_t bytes) { return (bytes + PAGE - 1) / PAGE; }

static bool allocate(uint32_t type, uint64_t pages, EFI_PHYSICAL_ADDRESS *address,
                     const char *why) {
    EFI_STATUS status = services->allocate_pages(type, EFI_LOADER_DATA, pages, address);
    return !EFI_ERROR(status) || fail(why, status);
}

static bool read_sectors(EFI_BLOCK_IO_PROTOCOL *io, uint64_t lba, uint64_t count, void *buffer) {
    EFI_STATUS status = io->read_blocks(io, io->media->media_id, lba, count * SECTOR, buffer);
    return !EFI_ERROR(status) || fail("the stick could not be read", status);
}

/* The unique GUID of the GPT partition the loader came from: its device
 * path's hard drive node. */
static bool boot_partition(EFI_HANDLE device, uint8_t guid[16]) {
    const unsigned char *node;
    EFI_STATUS status = services->handle_protocol(device, &device_path_guid, (void **)&node);
    if (EFI_ERROR(status)) return fail("the loader's device has no path", status);
    for (unsigned length; node[0] != DEVICE_PATH_END; node += length) {
        length = node[2] | node[3] << 8;
        if (length < 4) break;
        if (node[0] == DEVICE_PATH_MEDIA && node[1] == MEDIA_HARD_DRIVE && length >= 42 &&
            node[41] == 2) {
            memcpy(guid, node + 24, 16);
            return true;
        }
    }
    return fail("the loader was not started from a GPT partition", 0);
}

struct gpt {
    uint8_t disk_guid[16];
    uint64_t entries_lba;
    uint32_t count, size;
};

/* A whole disk's primary GPT, its header and entries checked by their
 * CRC-32s, into scratch: the header, then the entries from SECTOR on. */
static bool read_gpt(EFI_BLOCK_IO_PROTOCOL *io, unsigned char *scratch, struct gpt *gpt) {
    if (!read_sectors(io, 1, 1, scratch)) return false;
    uint32_t header_size, header_crc, entries_crc;
    memcpy(&header_size, scratch + 12, 4);
    memcpy(&header_crc, scratch + 16, 4);
    if (memcmp(scratch, "EFI PART", 8) || header_size < 92 || header_size > SECTOR) return false;
    memset(scratch + 16, 0, 4);
    if (crc32(scratch, header_size) != header_crc) return false;
    memcpy(gpt->disk_guid, scratch + 56, 16);
    memcpy(&gpt->entries_lba, scratch + 72, 8);
    memcpy(&gpt->count, scratch + 80, 4);
    memcpy(&gpt->size, scratch + 84, 4);
    memcpy(&entries_crc, scratch + 88, 4);
    uint64_t bytes = (uint64_t)gpt->count * gpt->size;
    if (gpt->size < 128 || gpt->size % 8 || !gpt->count || bytes > SCRATCH_PAGES * PAGE - SECTOR)
        return false;
    if (!read_sectors(io, gpt->entries_lba, (bytes + SECTOR - 1) / SECTOR, scratch + SECTOR))
        return false;
    return crc32(scratch + SECTOR, bytes) == entries_crc;
}

/* A GPT entry's name, UTF-16 at byte 56, is exactly name (ASCII). */
static bool partition_named(const unsigned char *entry, const char *name) {
    unsigned i = 0;
    for (; name[i]; ++i)
        if (entry[56 + 2 * i] != (unsigned char)name[i] || entry[57 + 2 * i]) return false;
    return i == 36 || (!entry[56 + 2 * i] && !entry[57 + 2 * i]);
}

struct system_disk {
    EFI_BLOCK_IO_PROTOCOL *io;
    uint8_t guid[16];
    uint64_t first, sectors; /* the sos2-system partition */
};

/* Among the whole disks, the one holding the loader's partition, and its
 * sos2-system partition. Copies of one stick share their GUIDs, so with two
 * of them which one the firmware started from is unknown: none is used. */
static bool find_system(EFI_HANDLE image, unsigned char *scratch, struct system_disk *out) {
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_STATUS status = services->handle_protocol(image, &loaded_image_guid, (void **)&loaded);
    if (EFI_ERROR(status)) return fail("the loader cannot see itself", status);
    uint8_t partition[16];
    if (!boot_partition(loaded->device_handle, partition)) return false;
    EFI_HANDLE *handles;
    uint64_t count;
    status = services->locate_handle_buffer(BY_PROTOCOL, &block_io_guid, 0, &count, &handles);
    if (EFI_ERROR(status)) return fail("the firmware lists no disks", status);
    unsigned found = 0;
    for (uint64_t i = 0; i < count && found < 2; ++i) {
        EFI_BLOCK_IO_PROTOCOL *io;
        struct gpt gpt;
        if (EFI_ERROR(services->handle_protocol(handles[i], &block_io_guid, (void **)&io)) ||
            io->media->logical_partition || !io->media->media_present ||
            io->media->block_size != SECTOR || !read_gpt(io, scratch, &gpt))
            continue;
        const unsigned char *system = 0;
        bool mine = false;
        for (uint32_t n = 0; n < gpt.count; ++n) {
            const unsigned char *entry = scratch + SECTOR + (uint64_t)n * gpt.size;
            if (!memcmp(entry + 16, partition, 16)) mine = true;
            if (partition_named(entry, "sos2-system")) system = entry;
        }
        if (!mine || ++found > 1) continue;
        if (!system) {
            fail("the stick has no sos2-system partition", 0);
            break;
        }
        uint64_t first, last;
        memcpy(&first, system + 32, 8);
        memcpy(&last, system + 40, 8);
        if (last < first || last > io->media->last_block) {
            fail("the sos2-system partition is invalid", 0);
            break;
        }
        out->io = io;
        memcpy(out->guid, gpt.disk_guid, 16);
        out->first = first;
        out->sectors = last - first + 1;
    }
    services->free_pool(handles);
    if (!found) return fail("the loader's own disk was not found", 0);
    if (found > 1) return fail("two sticks hold this system: start with one of them only", 0);
    return out->io != 0;
}

static bool terminated(const char *text, size_t size) {
    for (size_t i = 0; i < size; ++i)
        if (!text[i]) return true;
    return false;
}

/* The 4 KiB image header, checked: magic, version, CRC-32, and every part
 * inside the partition at a 4 KiB boundary, in order. */
static bool read_header(const struct system_disk *disk, struct pc_image_header *header) {
    if (!read_sectors(disk->io, disk->first, PC_IMAGE_HEADER / SECTOR, header)) return false;
    if (memcmp(header->magic, PC_IMAGE_MAGIC, 8))
        return fail("the sos2-system partition holds no system, as an interrupted update "
                    "leaves it: write the stick again",
                    0);
    uint32_t expected = header->header_crc;
    header->header_crc = 0;
    if (header->version != PC_IMAGE_VERSION || header->header_size != PC_IMAGE_HEADER ||
        crc32(header, PC_IMAGE_HEADER) != expected)
        return fail("the system image's header is damaged", 0);
    uint64_t space = disk->sectors * SECTOR;
    if (header->image_size > space || header->kernel_offset % PAGE ||
        header->ramdisk_offset % PAGE || header->kernel_offset < PC_IMAGE_HEADER ||
        header->kernel_size < 64 || header->kernel_offset > header->image_size ||
        header->kernel_size > header->image_size - header->kernel_offset ||
        header->ramdisk_offset < header->kernel_offset + header->kernel_size ||
        !header->ramdisk_size || header->ramdisk_offset > header->image_size ||
        header->ramdisk_size > header->image_size - header->ramdisk_offset ||
        !terminated(header->command_line, sizeof(header->command_line)))
        return fail("the system image's layout is invalid", 0);
    return true;
}

/* bytes of the image from offset into memory, in chunks, with a dot each. */
static bool read_part(const struct system_disk *disk, uint64_t offset, uint64_t bytes,
                      void *buffer) {
    unsigned char *to = buffer;
    uint64_t sectors = (bytes + SECTOR - 1) / SECTOR, lba = disk->first + offset / SECTOR;
    for (uint64_t done = 0; done < sectors;) {
        uint64_t n = sectors - done < READ_CHUNK / SECTOR ? sectors - done : READ_CHUNK / SECTOR;
        if (!read_sectors(disk->io, lba + done, n, to + done * SECTOR)) return false;
        done += n;
        print(".");
    }
    return true;
}

struct elf_header {
    unsigned char ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct program_header {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

/* The kernel's ELF, its loadable segments copied where they are linked
 * (one allocation from the lowest to the highest), the rest zeroed. */
static bool load_kernel(const unsigned char *file, uint64_t size, uint64_t *entry) {
    const struct elf_header *elf = (const void *)file;
    if (size < sizeof(*elf) ||
        memcmp(elf->ident,
               "\x7f"
               "ELF\x02\x01\x01",
               7) ||
        elf->type != 2 || elf->machine != 62 || elf->phentsize != sizeof(struct program_header) ||
        !elf->phnum || elf->phoff > size ||
        elf->phnum * sizeof(struct program_header) > size - elf->phoff)
        return fail("the kernel is not an x64 executable", 0);
    const struct program_header *headers = (const void *)(file + elf->phoff);
    uint64_t low = UINT64_MAX, high = 0;
    for (unsigned i = 0; i < elf->phnum; ++i) {
        const struct program_header *p = &headers[i];
        if (p->type != 1 || !p->memsz) continue;
        if (p->filesz > p->memsz || p->offset > size || p->filesz > size - p->offset ||
            p->paddr > BELOW_4G || p->memsz > BELOW_4G - p->paddr)
            return fail("the kernel's segments are invalid", 0);
        if (p->paddr < low) low = p->paddr;
        if (p->paddr + p->memsz > high) high = p->paddr + p->memsz;
    }
    if (low >= high || elf->entry < low || elf->entry >= high)
        return fail("the kernel has no entry", 0);
    EFI_PHYSICAL_ADDRESS base = low & ~(uint64_t)(PAGE - 1);
    uint64_t pages = pages_for(high - base);
    /* Code, which runs (from uefi_start) on the firmware's tables: firmware
     * that marks new pages no-execute or read-only, as it may for a loader
     * declaring NX_COMPAT, has them cleared through its memory attribute
     * protocol. */
    EFI_STATUS status = services->allocate_pages(ALLOCATE_ADDRESS, EFI_LOADER_CODE, pages, &base);
    if (EFI_ERROR(status)) {
        print("\nThe kernel runs at ");
        print_hex(low);
        print(" to ");
        print_hex(high);
        print(", where the firmware's memory is in use.\n");
        return fail("no room where the kernel runs", status);
    }
    EFI_MEMORY_ATTRIBUTE_PROTOCOL *attributes;
    if (!EFI_ERROR(services->locate_protocol(&memory_attribute_guid, 0, (void **)&attributes))) {
        const uint64_t locked = EFI_MEMORY_XP | EFI_MEMORY_RO;
        uint64_t left = 0;
        status = attributes->clear_memory_attributes(attributes, base, pages * PAGE, locked);
        if (EFI_ERROR(status) &&
            (EFI_ERROR(attributes->get_memory_attributes(attributes, base, pages * PAGE, &left)) ||
             (left & locked)))
            return fail("the firmware would not let the kernel run", status);
    }
    memset((void *)(uintptr_t)base, 0, pages * PAGE);
    for (unsigned i = 0; i < elf->phnum; ++i) {
        const struct program_header *p = &headers[i];
        if (p->type == 1 && p->filesz)
            memcpy((void *)(uintptr_t)p->paddr, file + p->offset, p->filesz);
    }
    *entry = elf->entry;
    return true;
}

/* Keep the firmware's mode unless it has fewer than MIN_LINES lines and a
 * taller 32-bit BGR mode exists, then the smallest of those. */
static bool choose_screen(struct sos2_boot_info *info) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_STATUS status = services->locate_protocol(&graphics_guid, 0, (void **)&gop);
    if (EFI_ERROR(status)) return fail("the firmware has no graphics output", status);
    if (gop->mode->info->vertical_resolution < MIN_LINES) {
        uint64_t best = 0;
        uint32_t best_mode = 0;
        for (uint32_t mode = 0; mode < gop->mode->max_mode; ++mode) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *m;
            uint64_t size;
            if (EFI_ERROR(gop->query_mode(gop, mode, &size, &m))) continue;
            uint64_t area = (uint64_t)m->horizontal_resolution * m->vertical_resolution;
            if (m->pixel_format == PIXEL_BLUE_GREEN_RED_RESERVED &&
                m->vertical_resolution >= MIN_LINES && (!best || area < best)) {
                best = area;
                best_mode = mode;
            }
            services->free_pool(m);
        }
        if (best) gop->set_mode(gop, best_mode);
    }
    const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *m = gop->mode->info;
    if (m->pixel_format != PIXEL_BLUE_GREEN_RED_RESERVED)
        return fail("the screen is not 32-bit blue-green-red, which SOS2 draws", 0);
    info->framebuffer = gop->mode->frame_buffer_base;
    info->framebuffer_size = gop->mode->frame_buffer_size;
    info->width = m->horizontal_resolution;
    info->height = m->vertical_resolution;
    info->stride = m->pixels_per_scan_line * 4;
    info->format = SOS2_PIXEL_XRGB8888;
    if (!info->width || !info->height || m->pixels_per_scan_line < m->horizontal_resolution ||
        (uint64_t)info->stride * info->height > info->framebuffer_size)
        return fail("the screen's framebuffer is invalid", 0);
    return true;
}

static uint64_t acpi_root(void) {
    const EFI_CONFIGURATION_TABLE *tables = system_table->configuration_table;
    for (uint64_t i = 0; i < system_table->table_count; ++i)
        if (same_guid(&tables[i].vendor_guid, &acpi20_guid))
            return (uintptr_t)tables[i].vendor_table;
    for (uint64_t i = 0; i < system_table->table_count; ++i)
        if (same_guid(&tables[i].vendor_guid, &acpi10_guid))
            return (uintptr_t)tables[i].vendor_table;
    return 0;
}

/* The firmware's memory map as the kernel's RAM, sorted and merged: what
 * the firmware's boot services used, the loader's allocations (the kernel
 * and its resources keep out of the page pool themselves) and free memory.
 * Everything else stays out of it. No boot service may be called here: it
 * runs between reading the map and leaving boot services. */
static bool kernel_memory_map(const unsigned char *map, uint64_t size, uint64_t descriptor_size,
                              struct sos2_boot_info *info) {
    uint32_t n = 0;
    struct sos2_memmap_entry *entries = info->memmap;
    for (uint64_t offset = 0; offset + descriptor_size <= size; offset += descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (const void *)(map + offset);
        if (d->type != EFI_LOADER_CODE && d->type != EFI_LOADER_DATA &&
            d->type != EFI_BOOT_SERVICES_CODE && d->type != EFI_BOOT_SERVICES_DATA &&
            d->type != EFI_CONVENTIONAL_MEMORY)
            continue;
        uint64_t start = d->physical_start, end = start + d->pages * PAGE;
        if (end <= start) continue;
        uint32_t at = 0;
        while (at < n && entries[at].address < start) ++at;
        if (at && entries[at - 1].address + entries[at - 1].size == start) {
            entries[at - 1].size += end - start;
            if (at < n && entries[at].address == end) {
                entries[at - 1].size += entries[at].size;
                for (uint32_t i = at; i + 1 < n; ++i) entries[i] = entries[i + 1];
                --n;
            }
            continue;
        }
        if (at < n && entries[at].address == end) {
            entries[at].address = start;
            entries[at].size += end - start;
            continue;
        }
        if (n == SOS2_BOOT_MEMMAP_MAX) return false;
        for (uint32_t i = n; i > at; --i) entries[i] = entries[i - 1];
        entries[at] = (struct sos2_memmap_entry){start, end - start, SOS2_MEMMAP_RAM, 0};
        ++n;
    }
    info->memmap_entries = n;
    return n != 0;
}

/* Everything up to leaving boot services; false with problem set when the
 * system cannot start. */
static bool start(EFI_HANDLE image) {
    EFI_PHYSICAL_ADDRESS scratch_address, header_address, info_address = BELOW_4G;
    if (!allocate(ALLOCATE_ANY_PAGES, SCRATCH_PAGES, &scratch_address,
                  "no memory for the loader") ||
        !allocate(ALLOCATE_ANY_PAGES, 1, &header_address, "no memory for the loader") ||
        !allocate(ALLOCATE_MAX_ADDRESS, pages_for(sizeof(struct sos2_boot_info)), &info_address,
                  "no memory below 4 GiB for the boot information"))
        return false;
    struct system_disk disk = {0};
    struct pc_image_header *header = (void *)(uintptr_t)header_address;
    if (!find_system(image, (unsigned char *)(uintptr_t)scratch_address, &disk) ||
        !read_header(&disk, header))
        return false;

    print("Loading SOS2");
    EFI_PHYSICAL_ADDRESS file, ramdisk = BELOW_4G;
    uint64_t entry;
    if (!allocate(ALLOCATE_ANY_PAGES, pages_for(header->kernel_size), &file,
                  "no memory for the kernel") ||
        !read_part(&disk, header->kernel_offset, header->kernel_size, (void *)(uintptr_t)file))
        return false;
    if (crc32((void *)(uintptr_t)file, header->kernel_size) != header->kernel_crc)
        return fail("the kernel is damaged", 0);
    if (!load_kernel((const unsigned char *)(uintptr_t)file, header->kernel_size, &entry))
        return false;
    services->free_pages(file, pages_for(header->kernel_size));
    if (!allocate(ALLOCATE_MAX_ADDRESS, pages_for(header->ramdisk_size), &ramdisk,
                  "no memory below 4 GiB for the system's resources") ||
        !read_part(&disk, header->ramdisk_offset, header->ramdisk_size, (void *)(uintptr_t)ramdisk))
        return false;
    if (crc32((void *)(uintptr_t)ramdisk, header->ramdisk_size) != header->ramdisk_crc)
        return fail("the system's resources are damaged", 0);
    print("\n");

    struct sos2_boot_info *info = (void *)(uintptr_t)info_address;
    memset(info, 0, sizeof(*info));
    info->magic = SOS2_BOOT_INFO_MAGIC;
    info->version = SOS2_BOOT_INFO_VERSION;
    info->size = sizeof(*info);
    info->rsdp = acpi_root();
    if (!info->rsdp) return fail("the firmware gives no ACPI tables", 0);
    info->module = ramdisk;
    info->module_size = header->ramdisk_size;
    memcpy(info->disk_guid, disk.guid, 16);
    memcpy(info->command_line, header->command_line, sizeof(info->command_line));
    if (!choose_screen(info)) return false;

    /* The map, then leaving boot services with its key; a map that changed
     * meanwhile is read again (nothing may be allocated in between). */
    uint64_t size = 0, key, descriptor_size;
    uint32_t version;
    unsigned char *map;
    EFI_STATUS status = services->get_memory_map(&size, 0, &key, &descriptor_size, &version);
    if (status != EFI_BUFFER_TOO_SMALL)
        return fail("the firmware's memory map cannot be read", status);
    uint64_t capacity = size + 32 * descriptor_size;
    status = services->allocate_pool(EFI_LOADER_DATA, capacity, (void **)&map);
    if (EFI_ERROR(status)) return fail("no memory for the memory map", status);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        size = capacity;
        status = services->get_memory_map(&size, (EFI_MEMORY_DESCRIPTOR *)map, &key,
                                          &descriptor_size, &version);
        if (EFI_ERROR(status)) return fail("the firmware's memory map cannot be read", status);
        if (!kernel_memory_map(map, size, descriptor_size, info))
            return fail("the firmware's memory map is too fragmented", 0);
        status = services->exit_boot_services(image, key);
        if (!EFI_ERROR(status)) {
            __asm__ volatile("cli\n\t"
                             "jmp *%0" ::"r"(entry),
                             "D"(info)
                             : "memory");
            __builtin_unreachable();
        }
    }
    return fail("the firmware would not hand over the machine", status);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *table) {
    system_table = table;
    services = table->boot_services;
    services->set_watchdog_timer(0, 0, 0, 0);
    print("SOS2\n");
    if (!start(image)) {
        print("\nSOS2 cannot start: ");
        print(problem);
        if (problem_status) {
            print(" (status ");
            print_hex(problem_status);
            print(")");
        }
        print(".\nPress a key to return to the firmware.\n");
        uint64_t index;
        table->con_in->reset(table->con_in, 0);
        services->wait_for_event(1, &table->con_in->wait_for_key, &index);
    }
    return problem_status ? problem_status : EFI_LOAD_ERROR;
}
