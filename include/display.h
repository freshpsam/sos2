#ifndef SOS2_DISPLAY_H
#define SOS2_DISPLAY_H
#include "fdt.h"
#include "user_abi.h"
/* Select the available QEMU display. Phone scanout is supplied by its loader. */
bool display_init(const struct platform_devices *, struct framebuffer *);
void display_poll(void);
void display_flush_error(void);
/* Standby: QEMU shows black; the phone turns its backlight off instead. */
void display_blank(void);
bool display_present_region(struct framebuffer *, const struct framebuffer *, unsigned x,
                            unsigned y, unsigned width, unsigned height);
bool display_present(struct framebuffer *, const struct framebuffer *);
/* Whether the display shows pages' GPU frames; false for good after a GPU
 * failure, and always on the phone. */
bool display_gpu_frames(void);
/* Show this region of the composition (its coordinates) again without copying
 * pixels: it shows a page whose GPU frames the display already holds. */
void display_damage(unsigned x, unsigned y, unsigned width, unsigned height);
/* The page host supplies trusted slot ownership and damage. UINT32_MAX selects
 * a desktop/overlay frame. The retained cache covers the full 480x752
 * viewport, of which the top height rows show; the composition supplies the
 * rest (the soft keyboard). Only GPU-frame pages are shown shorter. */
void display_page_view(unsigned owner, unsigned height);
void display_page_changed(unsigned owner, unsigned x, unsigned y, unsigned width, unsigned height);
void display_page_scroll(unsigned owner, const struct user_pixel_scroll *);
void display_page_closed(unsigned owner);
/* The page presents CPU pixels again, after GPU frames (USER_GPU_PRESENT). */
void display_page_cpu_frames(unsigned owner);
#endif
