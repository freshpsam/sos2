#ifndef SOS2_GRAPHICS_H
#define SOS2_GRAPHICS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Little-endian packed formats from the simple-framebuffer binding. */
enum pixel_format { PIXEL_XRGB8888, PIXEL_RGB565, PIXEL_RGB888, PIXEL_ARGB8888 };

struct framebuffer {
    void *pixels; /* Untyped storage: 16-, 24- and 32-bit surfaces are supported. */
    uint32_t width, height, stride; /* stride in bytes */
    enum pixel_format format;
};

/* Cancellation ends an uncertain gesture without treating it as a click. */
struct pointer_event {
    uint32_t x, y;
    bool down, cancelled;
};

bool gfx_framebuffer_valid(const struct framebuffer *fb);
void gfx_rect(struct framebuffer *fb, int x, int y, int w, int h, uint32_t color);
void gfx_text(struct framebuffer *fb, int x, int y, const char *text, unsigned scale,
              uint32_t color);
/* Width in pixels that gfx_text inks for the widest line of text: 5-pixel
 * glyphs a 6-pixel advance apart, times scale. Use it to center labels. */
int gfx_text_width(const char *text, unsigned scale);
/* Draw a width x height ARGB8888 image; pixels with alpha below 128 are
 * transparent and leave the surface untouched. */
void gfx_image(struct framebuffer *fb, int x, int y, int width, int height, const uint32_t *pixels);
/* Present a complete, non-overlapping native-format image. Only changed pixels
 * are written to the visible surface; padding is untouched. This is not vsync. */
bool gfx_present(struct framebuffer *visible, const struct framebuffer *completed);
/* Scale an XRGB8888 composition into any supported scanout format. */
bool gfx_present_scaled(struct framebuffer *visible, const struct framebuffer *completed);
/* Update only pixels whose nearest source sample lies in this source-space
 * rectangle. Bounds are clipped; untouched scanout pixels are not read. */
bool gfx_present_scaled_region(struct framebuffer *visible, const struct framebuffer *completed,
                               uint32_t x, uint32_t y, uint32_t width, uint32_t height);
#endif
