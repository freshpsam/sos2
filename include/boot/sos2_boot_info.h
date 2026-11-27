#ifndef SOS2_BOOT_INFO_H
#define SOS2_BOOT_INFO_H
#include <stdint.h>

/* What SOS2's UEFI loader (boot/uefi/loader.c) hands a PC's kernel
 * (arch/x64/machine.c) in place of PVH's start information: the memory
 * map, the resources (the system image's SORZ ramdisk), the sos2.* command
 * line, ACPI's root pointer, the GOP framebuffer and the GPT GUID of the
 * disk the system came from. The structure and everything it points to lie
 * below 4 GiB, which the kernel's boot map covers. */
#define SOS2_BOOT_INFO_MAGIC   0x4f464e4932534f53ull /* "SOS2INFO" */
#define SOS2_BOOT_INFO_VERSION 1
#define SOS2_BOOT_MEMMAP_MAX   256
#define SOS2_BOOT_COMMAND_LINE 256

/* PVH's e820 types: RAM, or kept from the kernel's use. */
enum { SOS2_MEMMAP_RAM = 1, SOS2_MEMMAP_RESERVED = 2 };

/* GOP's 32-bit blue, green, red, reserved: include/graphics.h's
 * PIXEL_XRGB8888. The loader accepts no other format. */
enum { SOS2_PIXEL_XRGB8888 = 1 };

struct sos2_memmap_entry {
    uint64_t address, size;
    uint32_t type, reserved;
};

struct sos2_boot_info {
    uint64_t magic;
    uint32_t version, size; /* size: the whole structure */
    uint64_t rsdp;
    uint64_t module, module_size;
    uint64_t framebuffer, framebuffer_size;
    uint32_t width, height, stride, format; /* stride in bytes */
    uint8_t disk_guid[16];
    char command_line[SOS2_BOOT_COMMAND_LINE];
    uint32_t memmap_entries, reserved;
    struct sos2_memmap_entry memmap[SOS2_BOOT_MEMMAP_MAX];
};
#endif
