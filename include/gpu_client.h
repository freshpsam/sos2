#ifndef SOS2_GPU_CLIENT_H
#define SOS2_GPU_CLIENT_H
#include "vm.h"
#include "user_abi.h"
/* SYS_GPU for one process (kernel/protection/gpu_client.c), whose page is
 * slot. *state starts null; close releases the page's context and resources
 * on process exit. */
struct gpu_client;
int gpu_client_call(struct gpu_client **state, const struct vm_space *space, unsigned slot,
                    uint32_t op, uint32_t address, uint32_t size);
void gpu_client_close(struct gpu_client **state);
/* Backing pages kept for good after an unconfirmed GPU reset: they never
 * return to the pool, so they do not count as a process page leak. */
unsigned gpu_client_quarantined_pages(void);
#endif
