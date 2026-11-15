#ifndef SOS2_NETWORK_H
#define SOS2_NETWORK_H
#include "fdt.h"
#include "vm.h"
void network_init(const struct platform_devices *);
void network_poll(void);
/* Initial nonblocking network/RNG preparation after the device comes online. */
bool network_startup_pending(void);
const char *network_status_detail(void);
bool network_service_pending(void);
void network_service_work(void *);
void network_close_process(unsigned pid);
int32_t network_call(unsigned pid, struct vm_space *, unsigned call, uint32_t a, uint32_t b,
                     uint32_t c);
#endif
