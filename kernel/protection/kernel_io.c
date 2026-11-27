#include "kernel_io.h"
#include "platform.h"
#include "arch/kernel_switch.h"

/* The first page is unmapped by vm_init. No user context runs on this stack. */
unsigned char kernel_io_stack[4096 + 65536] __attribute__((aligned(4096), section(".cpu_private")));
static uintptr_t caller_sp, worker_sp;
static void (*job)(void *);
static void *job_argument;
static bool busy, active;

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
    worker_sp = arch_kernel_frame(kernel_io_stack + sizeof(kernel_io_stack), worker_entry);
    busy = true;
}

void kernel_io_step(void) {
    if (!busy) return;
    active = true;
    kernel_switch(&caller_sp, worker_sp);
    active = false;
}

void kernel_io_wait(void) {
    if (active) kernel_switch(&worker_sp, caller_sp);
}
