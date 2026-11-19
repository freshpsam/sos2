#ifndef SOS2_QCOM_MDT_H
#define SOS2_QCOM_MDT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define QCOM_MDT_SEGMENTS 32
#define QCOM_MDT_METADATA (256u * 1024)
#define WCNSS_MEMORY_SIZE (6u * 1024 * 1024)

struct firmware_file {
    const unsigned char *data;
    size_t size;
};
typedef struct firmware_file (*firmware_lookup)(void *, const char *);

struct qcom_mdt_segment {
    const unsigned char *data;
    uint32_t offset, size, memory;
};

struct qcom_mdt {
    struct qcom_mdt_segment segments[QCOM_MDT_SEGMENTS];
    unsigned count, span;
};

/* Validation never writes firmware RAM or invokes the secure monitor. Only
 * relocatable ELF32/ARM MDT images with intact authentication metadata qualify. */
bool qcom_mdt_prepare(firmware_lookup, void *, struct qcom_mdt *, void *metadata, size_t capacity);
#endif
