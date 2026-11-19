#ifndef SOS2_QCOM_SMD_H
#define SOS2_QCOM_SMD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SMD_PACKET_MAX = 4096, SMD_OK = 1, SMD_AGAIN = 0, SMD_FAILED = -1, SMD_INVALID = -2 };

/* Vendor SMD accepts combined FIFO sizes divisible by 32, not just powers of
 * two. Bound them to the board's mapped SMEM window. */
static inline bool smd_fifo_size_valid(size_t n) {
    return n >= 64 && n <= 1024 * 1024 && n % 32 == 0;
}

/* A serialized, nonblocking packet channel. The board must supply exclusively
 * owned, uncached SMEM mappings after validating their allocation and waking
 * the remote processor. This layer neither maps RAM nor starts firmware. */
struct smd_channel {
    volatile unsigned char *info, *tx, *rx;
    unsigned size, half, head, tail, state;
    bool words, failed, peer_opening;
    uint64_t deadline;
    void (*signal)(void *);
    void *context;
};

bool smd_open(struct smd_channel *, volatile void *info, size_t info_size, volatile void *fifo,
              size_t fifo_size, void (*signal)(void *), void *context, uint64_t now);
/* Boot handoff only: adopt an already OPENED, completely drained channel.
 * Both endpoints' indices must be stable and valid. Never resets peer state. */
bool smd_adopt_idle(struct smd_channel *, volatile void *, size_t, volatile void *, size_t,
                    void (*)(void *), void *);
int smd_poll(struct smd_channel *, uint64_t now);
int smd_send(struct smd_channel *, const void *data, unsigned size, uint64_t now);
/* A complete packet is consumed only if it fits. *size is zero on AGAIN/error. */
int smd_receive(struct smd_channel *, void *data, unsigned capacity, unsigned *size, uint64_t now);
void smd_close(struct smd_channel *);
#endif
