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
/* display_present_region in two steps, for a region drawn a band of rows at a
 * time (kernel/protection/page_host.c): each band drawn, then the region shown
 * once all are, so that a panel sent frames on demand (the Nexus 5's) never
 * gets half of one. display_bands is false where the display presents regions
 * itself (the Redmi 2's Adreno): there they are presented whole. */
bool display_bands(void);
bool display_draw_region(struct framebuffer *, const struct framebuffer *, unsigned x, unsigned y,
                         unsigned width, unsigned height);
void display_show_region(struct framebuffer *, const struct framebuffer *, unsigned x, unsigned y,
                         unsigned width, unsigned height);
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
