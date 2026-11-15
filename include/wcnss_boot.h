#ifndef SOS2_WCNSS_BOOT_H
#define SOS2_WCNSS_BOOT_H
#include "qcom_mdt.h"
#include "qcom_smem.h"
#include "qcom_rpm.h"
#include "wcnss_control.h"

struct wcnss_io {
    uint32_t (*read)(void *, uintptr_t);
    void (*write)(void *, uintptr_t, uint32_t);
    void (*signal)(void *, uint32_t);
    void *context;
    /* Optional worker callback: publish the stage and yield before hardware access. */
    void (*progress)(void *, const char *);
};

enum wcnss_boot_state {
    WB_OFF,
    WB_SCM,
    WB_RPM,
    WB_POWER,
    WB_CLOCKS,
    WB_CLOCK_PLL,
    WB_CLOCK_UPDATE,
    WB_CLOCK_READY,
    WB_METADATA,
    WB_MEMORY,
    WB_COPY,
    WB_SMP2P,
    WB_IRIS_RESET,
    WB_IRIS_READ,
    WB_IRIS_CONFIG,
    WB_IRIS_XO,
    WB_SETTLE,
    WB_AUTH,
    WB_READY_WAIT,
    WB_CONTROL,
    WB_RUNNING,
    WB_FAILED
};

struct wcnss_boot {
    enum wcnss_boot_state state;
    const char *error, *status;
    char diagnostic[64];
    /* Stock SMEM crash reason, snapshotted before PAS starts. Never clear the
     * remote's buffer or report an inherited reason as a new firmware fault. */
    char prior_crash[81];
    /* Optional, sanitized firmware context. Never replaces the primary error. */
    char firmware_detail[96];
    struct wcnss_io io;
    struct smem_region *smem;
    struct smd_channel rpm_channel, control_channel;
    struct rpm_request rpm;
    struct wcnss_control control;
    struct qcom_mdt plan;
    struct firmware_file nv;
    struct smem_item smp_out;
    unsigned char *memory, *metadata;
    uintptr_t physical, metadata_physical;
    unsigned copied, vote, iris_attempts;
    uint32_t iris_id;
    uint64_t deadline;
    bool authenticated, control_open, pas_initialized, smp_seen, crash_baseline,
        watchdog_clear_seen;
    bool clock_scaling; /* The bus-clock votes and the RPM's scaling are in. */
};

bool wcnss_boot_begin(struct wcnss_boot *, firmware_lookup, void *, struct smem_region *,
                      const struct wcnss_io *, void *memory, uintptr_t physical, void *metadata,
                      uintptr_t metadata_physical, uint64_t now);
enum wcnss_boot_state wcnss_boot_poll(struct wcnss_boot *, uint64_t now);
void wcnss_boot_stop(struct wcnss_boot *);
#endif
