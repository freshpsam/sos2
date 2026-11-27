#ifndef SOS2_EFI_H
#define SOS2_EFI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The parts of UEFI's interfaces SOS2's loader uses, as the UEFI 2.10
 * specification lays them out (https://uefi.org/specifications). Firmware
 * calls use the Microsoft x64 convention; BOOLEAN is a byte. */
#define EFIAPI __attribute__((ms_abi))
typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef uint16_t CHAR16;
typedef uint8_t BOOLEAN;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_LBA;

#define EFI_SUCCESS           0
#define EFI_ERROR(status)     (((status) >> 63) != 0)
#define EFI_LOAD_ERROR        (1ull << 63 | 1)
#define EFI_INVALID_PARAMETER (1ull << 63 | 2)
#define EFI_BUFFER_TOO_SMALL  (1ull << 63 | 5)

typedef struct {
    uint32_t data1;
    uint16_t data2, data3;
    uint8_t data4[8];
} EFI_GUID;

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}
#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    {0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    {0x964e5b21, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}}
#define EFI_ACPI_20_TABLE_GUID \
    {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}}
#define EFI_ACPI_10_TABLE_GUID \
    {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}}
#define EFI_MEMORY_ATTRIBUTE_PROTOCOL_GUID \
    {0xf4560cf6, 0x40ec, 0x4b4a, {0xa1, 0x92, 0xbf, 0x1d, 0x57, 0xd0, 0xb1, 0x89}}

/* Memory attributes (UEFI 2.10, 7.2 and 37.7). */
#define EFI_MEMORY_XP 0x4000ull
#define EFI_MEMORY_RO 0x20000ull

typedef struct {
    uint64_t signature;
    uint32_t revision, header_size, crc32, reserved;
} EFI_TABLE_HEADER;

typedef struct {
    uint16_t scan_code;
    CHAR16 unicode_char;
} EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS(EFIAPI *reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self, BOOLEAN extended);
    EFI_STATUS(EFIAPI *read_key_stroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *self,
                                        EFI_INPUT_KEY *key);
    EFI_EVENT wait_for_key;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS(EFIAPI *reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, BOOLEAN extended);
    EFI_STATUS(EFIAPI *output_string)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self,
                                      const CHAR16 *text);
    void *test_string, *query_mode, *set_mode, *set_attribute;
    EFI_STATUS(EFIAPI *clear_screen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self);
    void *set_cursor_position, *enable_cursor, *mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* Pages' attributes, where the firmware enforces them (UEFI 2.10, 37.7). */
typedef struct EFI_MEMORY_ATTRIBUTE_PROTOCOL {
    EFI_STATUS(EFIAPI *get_memory_attributes)(struct EFI_MEMORY_ATTRIBUTE_PROTOCOL *self,
                                              EFI_PHYSICAL_ADDRESS base, uint64_t length,
                                              uint64_t *attributes);
    EFI_STATUS(EFIAPI *set_memory_attributes)(struct EFI_MEMORY_ATTRIBUTE_PROTOCOL *self,
                                              EFI_PHYSICAL_ADDRESS base, uint64_t length,
                                              uint64_t attributes);
    EFI_STATUS(EFIAPI *clear_memory_attributes)(struct EFI_MEMORY_ATTRIBUTE_PROTOCOL *self,
                                                EFI_PHYSICAL_ADDRESS base, uint64_t length,
                                                uint64_t attributes);
} EFI_MEMORY_ATTRIBUTE_PROTOCOL;

enum { ALLOCATE_ANY_PAGES, ALLOCATE_MAX_ADDRESS, ALLOCATE_ADDRESS };

enum {
    EFI_RESERVED_MEMORY_TYPE,
    EFI_LOADER_CODE,
    EFI_LOADER_DATA,
    EFI_BOOT_SERVICES_CODE,
    EFI_BOOT_SERVICES_DATA,
    EFI_RUNTIME_SERVICES_CODE,
    EFI_RUNTIME_SERVICES_DATA,
    EFI_CONVENTIONAL_MEMORY,
};

enum { BY_PROTOCOL = 2 };

typedef struct {
    uint32_t type, pad;
    EFI_PHYSICAL_ADDRESS physical_start;
    uint64_t virtual_start, pages, attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    EFI_TABLE_HEADER header;
    void *raise_tpl, *restore_tpl;
    EFI_STATUS(EFIAPI *allocate_pages)(uint32_t type, uint32_t memory_type, uint64_t pages,
                                       EFI_PHYSICAL_ADDRESS *memory);
    EFI_STATUS(EFIAPI *free_pages)(EFI_PHYSICAL_ADDRESS memory, uint64_t pages);
    EFI_STATUS(EFIAPI *get_memory_map)(uint64_t *size, EFI_MEMORY_DESCRIPTOR *map, uint64_t *key,
                                       uint64_t *descriptor_size, uint32_t *descriptor_version);
    EFI_STATUS(EFIAPI *allocate_pool)(uint32_t memory_type, uint64_t size, void **buffer);
    EFI_STATUS(EFIAPI *free_pool)(void *buffer);
    void *create_event, *set_timer;
    EFI_STATUS(EFIAPI *wait_for_event)(uint64_t count, EFI_EVENT *events, uint64_t *index);
    void *signal_event, *close_event, *check_event;
    void *install_protocol_interface, *reinstall_protocol_interface, *uninstall_protocol_interface;
    EFI_STATUS(EFIAPI *handle_protocol)(EFI_HANDLE handle, const EFI_GUID *protocol,
                                        void **interface);
    void *reserved, *register_protocol_notify, *locate_handle, *locate_device_path;
    void *install_configuration_table;
    void *load_image, *start_image, *exit, *unload_image;
    EFI_STATUS(EFIAPI *exit_boot_services)(EFI_HANDLE image, uint64_t map_key);
    void *get_next_monotonic_count;
    EFI_STATUS(EFIAPI *stall)(uint64_t microseconds);
    EFI_STATUS(EFIAPI *set_watchdog_timer)(uint64_t timeout, uint64_t code, uint64_t data_size,
                                           const CHAR16 *data);
    void *connect_controller, *disconnect_controller;
    void *open_protocol, *close_protocol, *open_protocol_information;
    void *protocols_per_handle;
    EFI_STATUS(EFIAPI *locate_handle_buffer)(uint32_t search, const EFI_GUID *protocol, void *key,
                                             uint64_t *count, EFI_HANDLE **handles);
    EFI_STATUS(EFIAPI *locate_protocol)(const EFI_GUID *protocol, void *registration,
                                        void **interface);
    void *install_multiple_protocol_interfaces, *uninstall_multiple_protocol_interfaces;
    EFI_STATUS(EFIAPI *calculate_crc32)(const void *data, uint64_t size, uint32_t *crc);
    void *copy_mem, *set_mem, *create_event_ex;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_GUID vendor_guid;
    void *vendor_table;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER header;
    const CHAR16 *firmware_vendor;
    uint32_t firmware_revision;
    EFI_HANDLE console_in_handle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *con_in;
    EFI_HANDLE console_out_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con_out;
    EFI_HANDLE standard_error_handle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *std_err;
    void *runtime_services;
    EFI_BOOT_SERVICES *boot_services;
    uint64_t table_count;
    EFI_CONFIGURATION_TABLE *configuration_table;
} EFI_SYSTEM_TABLE;

typedef struct {
    uint32_t revision;
    EFI_HANDLE parent_handle;
    EFI_SYSTEM_TABLE *system_table;
    EFI_HANDLE device_handle;
    void *file_path, *reserved;
    uint32_t load_options_size;
    void *load_options, *image_base;
    uint64_t image_size;
    uint32_t image_code_type, image_data_type;
    void *unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* A device path is a run of nodes: type, subtype and a 16-bit length, each
 * node that many bytes. The hard drive node (media, 1) carries its GPT
 * partition's unique GUID at byte 24, with signature type 2. */
enum { DEVICE_PATH_MEDIA = 4, DEVICE_PATH_END = 0x7f, MEDIA_HARD_DRIVE = 1 };

typedef struct {
    uint32_t media_id;
    BOOLEAN removable_media, media_present, logical_partition, read_only, write_caching;
    uint32_t block_size, io_align;
    EFI_LBA last_block;
} EFI_BLOCK_IO_MEDIA;

typedef struct EFI_BLOCK_IO_PROTOCOL {
    uint64_t revision;
    EFI_BLOCK_IO_MEDIA *media;
    void *reset;
    EFI_STATUS(EFIAPI *read_blocks)(struct EFI_BLOCK_IO_PROTOCOL *self, uint32_t media_id,
                                    EFI_LBA lba, uint64_t size, void *buffer);
    void *write_blocks, *flush_blocks;
} EFI_BLOCK_IO_PROTOCOL;

enum {
    PIXEL_RED_GREEN_BLUE_RESERVED,
    PIXEL_BLUE_GREEN_RED_RESERVED,
    PIXEL_BIT_MASK,
    PIXEL_BLT_ONLY
};

typedef struct {
    uint32_t version, horizontal_resolution, vertical_resolution, pixel_format;
    uint32_t red_mask, green_mask, blue_mask, reserved_mask;
    uint32_t pixels_per_scan_line;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t max_mode, mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    uint64_t size_of_info;
    EFI_PHYSICAL_ADDRESS frame_buffer_base;
    uint64_t frame_buffer_size;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS(EFIAPI *query_mode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *self, uint32_t mode,
                                   uint64_t *size, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **info);
    EFI_STATUS(EFIAPI *set_mode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *self, uint32_t mode);
    void *blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;
#endif
