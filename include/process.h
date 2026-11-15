#ifndef SOS2_PROCESS_H
#define SOS2_PROCESS_H
#include "fdt.h"

/* Layout shared with arch/arm/process.S; CPSR is never supplied by a syscall. */
struct arm_context {
    uint32_t r[13], sp, lr, pc, cpsr;
};

struct arm_trap {
    struct arm_context context;
    uint32_t vector, status, address;
};

struct arm_runtime_context {
    uint64_t d[32];
    uint32_t fpscr, tls, tls_ro, padding;
};

void arm_runtime_init(void);
void arm_runtime_save(struct arm_runtime_context *context);
void arm_runtime_restore(const struct arm_runtime_context *context);
void arm_user_run(struct arm_context *context);
void arm_trap_dispatch(struct arm_trap *trap);
bool process_init(struct platform_devices *devices, uintptr_t dtb);
/* Collect worker completions and dispatch bounded slices between UI iterations.
 * On one CPU, executes one slice locally. Kernel services stay on the boot CPU. */
void process_step(void);
/* Standby: stop starting user slices; kernel services keep running. */
void process_freeze(bool freeze);
/* Trusted shell only. Close waits for in-flight slices before reclaiming RAM. */
int32_t process_web_control(unsigned slot, bool restart);
/* Allocate a new isolated page; returns its zero-based identifier or an error. */
int32_t process_web_open(void);
/* Start an app (include/apps.h) in a new slot, like a page; same results. */
struct app;
int32_t process_app_open(const struct app *app);
/* Whether a process, even one still being reclaimed, runs this app. */
bool process_app_running(const struct app *app);
#endif
