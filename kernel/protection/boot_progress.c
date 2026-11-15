#include "boot_progress.h"
static boot_progress_handler handler;
static enum boot_phase phase;
static uint32_t completed, total;

void boot_progress_set_handler(boot_progress_handler next) { handler = next; }

void boot_progress_report(enum boot_phase next, uint32_t done, uint32_t size) {
    phase = next;
    total = size;
    completed = done > size ? size : done;
    boot_progress_poll();
}

void boot_progress_poll(void) {
    if (handler) handler(phase, completed, total);
}
