#ifndef SOS2_BOOT_PROGRESS_H
#define SOS2_BOOT_PROGRESS_H
#include <stdint.h>

/* In startup order; the phone's loading screen fills one bar across them. */
enum boot_phase {
    BOOT_CONTROLS,
    BOOT_CHECK_ENGINE,
    BOOT_LOAD_ENGINE,
    BOOT_VERIFY_ENGINE,
    BOOT_MEMORY,
    BOOT_STORAGE,
    BOOT_SECURITY,
    BOOT_NETWORK,
    BOOT_CLOCK,
    BOOT_WIFI,
};

typedef void (*boot_progress_handler)(enum boot_phase, uint32_t, uint32_t);
/* Only the trusted startup screen runs from this hook. It must not allocate,
 * access files, start processes or re-enter a partially initialized service. */
void boot_progress_set_handler(boot_progress_handler);
void boot_progress_report(enum boot_phase, uint32_t completed, uint32_t total);
void boot_progress_poll(void);
#endif
