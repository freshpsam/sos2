#include "kernel_io.h"
#include "platform.h"

/* The first page is unmapped by vm_init. No user context runs on this stack. */
unsigned char kernel_io_stack[4096 + 65536] __attribute__((aligned(4096), section(".cpu_private")));
static uintptr_t caller_sp, worker_sp;
static void (*job)(void *);
static void *job_argument;
static bool busy, active;
extern void arm_kernel_switch(uintptr_t *save, uintptr_t restore);

bool kernel_io_busy(void) { return busy; }

static _Noreturn void worker_entry(void) {
    job(job_argument);
    busy = false;
    kernel_io_wait();
    panic("completed service worker resumed");
}

void kernel_io_start(void (*work)(void *), void *argument) {
    if (busy || active) panic("service worker already active");
    job = work;
    job_argument = argument;
    /* arm_kernel_switch pops r4-r11, r12 (alignment), and pc. */
    uint32_t *frame = (uint32_t *)(kernel_io_stack + sizeof(kernel_io_stack)) - 10;
    memset(frame, 0, 10 * sizeof(*frame));
    frame[9] = (uintptr_t)worker_entry;
    worker_sp = (uintptr_t)frame;
    busy = true;
}

void kernel_io_step(void) {
    if (!busy) return;
    active = true;
    arm_kernel_switch(&caller_sp, worker_sp);
    active = false;
}

void kernel_io_wait(void) {
    if (active) arm_kernel_switch(&worker_sp, caller_sp);
}
