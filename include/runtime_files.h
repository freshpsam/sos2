#ifndef SOS2_RUNTIME_FILES_H
#define SOS2_RUNTIME_FILES_H
#include "fdt.h"
#define RUNTIME_FILES_BASE  0x50000000u
#define RUNTIME_FILES_LIMIT 0x60000000u

struct runtime_file {
    char name[128];
    uint32_t offset, size;
};

bool runtime_files_init(struct platform_devices *devices, uintptr_t dtb, uintptr_t kernel_start,
                        uintptr_t kernel_end);
uintptr_t runtime_files_base(void);
uint32_t runtime_files_size(void);
unsigned runtime_files_count(void);
const struct runtime_file *runtime_file_at(unsigned index);
const struct runtime_file *runtime_file_find(const char *path);
const void *runtime_file_data(const struct runtime_file *file);
#endif
