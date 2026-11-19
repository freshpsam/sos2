#ifndef SOS2_REDMI2_ADRENO_H
#define SOS2_REDMI2_ADRENO_H
#include "graphics.h"
struct user_pixel_scroll;
/* Called once while the loading screen is still visible. Missing/unsupported
 * hardware or a failed readback leaves normal CPU presentation available. */
void redmi2_adreno_init(struct framebuffer *);
void redmi2_adreno_poll(void);
void redmi2_adreno_stop(void);
/* Standby rests an idle GPU, its power domain off as KGSL's slumber, else its
 * core clocks stopped (KGSL's nap); waking restarts it. A busy or absent GPU
 * keeps its clocks. Returns whether the GPU rests. */
bool redmi2_adreno_standby(bool nap);
/* No GPU job can run: the GPU rests in standby, failed or never started.
 * Whole-chip sleep waits for it, as Linux suspends KGSL first. */
bool redmi2_adreno_resting(void);
void redmi2_adreno_page_view(unsigned owner);
void redmi2_adreno_page_changed(unsigned owner, unsigned x, unsigned y, unsigned width,
                                unsigned height);
void redmi2_adreno_page_scroll(unsigned owner, const struct user_pixel_scroll *);
void redmi2_adreno_page_closed(unsigned owner);

/* The caller retains this kernel-owned composition for the display lifetime;
 * updates between completed GPU jobs are coalesced. */
bool redmi2_adreno_present(struct framebuffer *, const struct framebuffer *, unsigned, unsigned,
                           unsigned, unsigned);
#endif
