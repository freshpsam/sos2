#ifndef SOS2_PC_IMAGE_H
#define SOS2_PC_IMAGE_H
#include <stdint.h>

/* A PC's system image, in its stick's sos2-system partition and, after the
 * update header, in an update (include/system_store.h): this 4 KiB header,
 * the kernel's ELF and the resources (a SORZ ramdisk), each at a 4 KiB
 * boundary. tools/package_pc.py writes it, SOS2's UEFI loader starts it
 * (boot/uefi/loader.c) and the kernel checks an update's before writing it
 * (kernel/protection/system_update.c). An update clears the partition's
 * first sector first and writes it last, so an interrupted one leaves no
 * magic for the loader to start. CRC-32s are zlib's. */
#define PC_IMAGE_MAGIC        "SOS2PCIM"
#define PC_IMAGE_VERSION      1
#define PC_IMAGE_HEADER       4096
#define PC_IMAGE_COMMAND_LINE 256

struct pc_image_header {
    char magic[8];
    uint32_t version, header_size;
    uint64_t kernel_offset, kernel_size;
    uint64_t ramdisk_offset, ramdisk_size;
    uint64_t image_size;
    uint32_t kernel_crc, ramdisk_crc;
    char command_line[PC_IMAGE_COMMAND_LINE];
    uint32_t header_crc; /* of the whole 4 KiB header with this field zero */
};
#endif
