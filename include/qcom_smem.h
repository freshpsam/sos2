#ifndef SOS2_QCOM_SMEM_H
#define SOS2_QCOM_SMEM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct smem_region {
    volatile unsigned char *base;
    size_t size;
    bool (*lock)(void *);
    void (*unlock)(void *);
    void *context;
    uintptr_t physical, aux_physical;
    volatile unsigned char *aux;
    size_t aux_size;
    /* Last serialized operation's validation failure; empty on success/busy.
     * Copy before another lookup if it must survive startup cleanup. */
    char error[64];
};

struct smem_item {
    volatile unsigned char *data;
    unsigned size;
};

enum smem_status { SMEM_INVALID = -1, SMEM_ABSENT = 0, SMEM_FOUND = 1, SMEM_BUSY = 2 };

enum { SMEM_HOST_GLOBAL = 0xffff, SMEM_HOST_WCNSS = 4, SMEM_HOST_RPM = 6 };
/* Detailed lookup for diagnostics: lock contention is not an absent item. */
enum smem_status smem_get_status(struct smem_region *, unsigned id, struct smem_item *);
/* 1 = found/allocated, 0 = not ready/lock busy, -1 = malformed. */
int smem_get(struct smem_region *, unsigned id, struct smem_item *);
/* Stock peer-scoped, uncached items. Use the APPS/peer partition when present;
 * fall back to the global heap only if no partition exists for that peer.
 * Global SMSM and crash-reason items continue to use the APIs above. */
enum smem_status smem_get_peer_status(struct smem_region *, unsigned peer, unsigned id,
                                      struct smem_item *);
int smem_allocate_peer(struct smem_region *, unsigned peer, unsigned id, unsigned size,
                       struct smem_item *);
int smem_smd_find(struct smem_region *, unsigned edge, const char *name, struct smem_item *info,
                  struct smem_item *fifo);
#endif
