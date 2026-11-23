#ifndef SOS2_VOLUME_KEYS_H
#define SOS2_VOLUME_KEYS_H
#include <stdbool.h>
#include <stdint.h>

/* A board's two volume keys (the tablet's keys.c, the Redmi 2's keys.c),
 * which held samples: +1 while Volume Up alone is held, -1 for Volume Down,
 * 0 for neither or both. They are sampled every 20 ms and taken once steady
 * for 40 ms. A press steps once; a key held half a second steps again every
 * 150 ms; keys held through the boot step nothing. */
struct volume_keys {
    uint64_t sampled, since, stepped;
    int candidate, steady;
    bool started, repeating;
};

/* From the main loop: +1 for a Volume Up step, -1 for Volume Down, 0
 * otherwise. */
int volume_keys_step(struct volume_keys *, int (*held)(void));
#endif
