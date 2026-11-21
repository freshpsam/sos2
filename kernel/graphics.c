#include "graphics.h"
#include <limits.h>

/* Hand-drawn 5 by 7 glyphs. Each row uses its five low bits, left to right. */
static const uint8_t letters[26][7] = {
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
    {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
    {7, 2, 2, 2, 18, 18, 12},     {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 25, 25, 21, 19, 19, 17}, {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31}};
/* Preserve text casing, including the lowercase sos2 name. */
static const uint8_t lowercase[26][7] = {
    {0, 0, 14, 1, 15, 17, 15},  {16, 16, 30, 17, 17, 17, 30}, {0, 0, 14, 16, 16, 17, 14},
    {1, 1, 15, 17, 17, 17, 15}, {0, 0, 14, 17, 31, 16, 14},   {6, 9, 8, 28, 8, 8, 8},
    {0, 0, 15, 17, 15, 1, 14},  {16, 16, 30, 17, 17, 17, 17}, {4, 0, 12, 4, 4, 4, 14},
    {2, 0, 6, 2, 2, 18, 12},    {16, 16, 18, 20, 24, 20, 18}, {12, 4, 4, 4, 4, 4, 14},
    {0, 0, 26, 21, 21, 21, 21}, {0, 0, 30, 17, 17, 17, 17},   {0, 0, 14, 17, 17, 17, 14},
    {0, 0, 30, 17, 30, 16, 16}, {0, 0, 15, 17, 15, 1, 1},     {0, 0, 22, 25, 16, 16, 16},
    {0, 0, 15, 16, 14, 1, 30},  {8, 8, 28, 8, 8, 9, 6},       {0, 0, 17, 17, 17, 19, 13},
    {0, 0, 17, 17, 17, 10, 4},  {0, 0, 17, 17, 21, 21, 10},   {0, 0, 17, 10, 4, 10, 17},
    {0, 0, 17, 17, 15, 1, 14},  {0, 0, 31, 2, 4, 8, 31}};
static const uint8_t digits[10][7] = {{14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},
                                      {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
                                      {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},
                                      {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
                                      {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14}};

static unsigned bytes_per_pixel(enum pixel_format format) {
    switch (format) {
        case PIXEL_RGB565: return 2;
        case PIXEL_RGB888: return 3;
        case PIXEL_XRGB8888:
        case PIXEL_ARGB8888: return 4;
        default: return 0;
    }
}

bool gfx_framebuffer_valid(const struct framebuffer *fb) {
    if (!fb || !fb->pixels || !fb->width || !fb->height) return false;
    unsigned bytes = bytes_per_pixel(fb->format);
    return bytes && (bytes == 3 || (!(fb->stride % bytes) && !((uintptr_t)fb->pixels % bytes))) &&
           fb->stride / bytes >= fb->width && (uint64_t)fb->stride * fb->height <= SIZE_MAX &&
           (uint64_t)fb->stride * fb->height <= UINTPTR_MAX - (uintptr_t)fb->pixels;
}

void gfx_rect(struct framebuffer *fb, int x, int y, int w, int h, uint32_t color) {
    if (!gfx_framebuffer_valid(fb) || w <= 0 || h <= 0) return;
    int64_t left = x < 0 ? 0 : x;
    int64_t top = y < 0 ? 0 : y;
    int64_t right = (int64_t)x + w;
    int64_t bottom = (int64_t)y + h;
    if (right > fb->width) right = fb->width;
    if (bottom > fb->height) bottom = fb->height;
    if (right <= left || bottom <= top) return;
    unsigned bytes = bytes_per_pixel(fb->format);
    color &= 0xffffff;
    if (fb->format == PIXEL_ARGB8888) color |= 0xff000000;
    uint16_t rgb565 =
        (uint16_t)(((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 0x1f));
    for (int64_t row = top; row < bottom; ++row) {
        uint8_t *pixels = (uint8_t *)fb->pixels + (size_t)row * fb->stride;
        for (int64_t col = left; col < right; ++col) {
            if (bytes == 4) ((uint32_t *)pixels)[col] = color;
            else if (bytes == 2) ((uint16_t *)pixels)[col] = rgb565;
            else {
                pixels[col * 3] = (uint8_t)color;
                pixels[col * 3 + 1] = (uint8_t)(color >> 8);
                pixels[col * 3 + 2] = (uint8_t)(color >> 16);
            }
        }
    }
}

void gfx_image(struct framebuffer *fb, int x, int y, int width, int height,
               const uint32_t *pixels) {
    if (!gfx_framebuffer_valid(fb) || !pixels || width <= 0 || height <= 0) return;
    for (int row = 0; row < height; ++row) {
        int64_t py = (int64_t)y + row;
        if (py < 0 || py >= fb->height) continue;
        for (int col = 0; col < width; ++col) {
            uint32_t argb = pixels[(size_t)row * (size_t)width + (size_t)col];
            int64_t px = (int64_t)x + col;
            if ((argb >> 31) && px >= 0 && px < fb->width)
                gfx_rect(fb, (int)px, (int)py, 1, 1, argb);
        }
    }
}

static uint8_t glyph_row(unsigned char c, unsigned row) {
    if (c >= 'a' && c <= 'z') return lowercase[c - 'a'][row];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'][row];
    if (c >= '0' && c <= '9') return digits[c - '0'][row];
    switch (c) {
        case ' ': return 0;
        case '-': return row == 3 ? 14 : 0;
        case '+': return row == 3 ? 31 : (row > 0 && row < 6 ? 4 : 0);
        case '=': return row == 2 || row == 4 ? 31 : 0;
        case '_': return row == 6 ? 31 : 0;
        case '*': {
            static const uint8_t g[] = {0, 4, 21, 14, 21, 4, 0};
            return g[row];
        }
        case '"': return row < 3 ? 10 : 0;
        case '`': return row == 0 ? 8 : row == 1 ? 4 : 0;
        case '~': {
            static const uint8_t g[] = {0, 0, 9, 22, 0, 0, 0};
            return g[row];
        }
        case '[': return row == 0 || row == 6 ? 14 : 8;
        case ']': return row == 0 || row == 6 ? 14 : 2;
        case '{': {
            static const uint8_t g[] = {2, 4, 4, 8, 4, 4, 2};
            return g[row];
        }
        case '}': {
            static const uint8_t g[] = {8, 4, 4, 2, 4, 4, 8};
            return g[row];
        }
        case '<': {
            static const uint8_t g[] = {0, 2, 4, 8, 4, 2, 0};
            return g[row];
        }
        case '>': {
            static const uint8_t g[] = {0, 8, 4, 2, 4, 8, 0};
            return g[row];
        }
        case '\\': return row < 5 ? (uint8_t)(16u >> row) : 0;
        case '.': return row == 6 ? 4 : 0;
        case ',': return row == 5 ? 4 : row == 6 ? 8 : 0;
        case ':': return row == 2 || row == 5 ? 4 : 0;
        case ';': return row == 2 || row == 5 ? 4 : row == 6 ? 8 : 0;
        case '!': return row < 5 || row == 6 ? 4 : 0;
        case '\'': return row < 2 ? 4 : row == 2 ? 8 : 0;
        case '(': return row == 0 || row == 6 ? 2 : row == 1 || row == 5 ? 4 : 8;
        case ')': return row == 0 || row == 6 ? 8 : row == 1 || row == 5 ? 4 : 2;
        case '|': return 4;
        case '^': return row == 0 ? 4 : row == 1 ? 10 : row == 2 ? 17 : 0;
        case '#': return row == 2 || row == 4 ? 31 : 10;
        case '$': {
            static const uint8_t g[] = {4, 15, 20, 14, 5, 30, 4};
            return g[row];
        }
        case '%': {
            static const uint8_t g[] = {25, 25, 2, 4, 8, 19, 19};
            return g[row];
        }
        case '&': {
            static const uint8_t g[] = {12, 18, 20, 8, 21, 18, 13};
            return g[row];
        }
        case '@': {
            static const uint8_t g[] = {14, 17, 23, 21, 23, 16, 14};
            return g[row];
        }
        case '/': return row < 5 ? (uint8_t)(1u << row) : 0;
        default: {
            static const uint8_t question[7] = {14, 17, 1, 2, 4, 0, 4};
            return question[row];
        }
    }
}

void gfx_text(struct framebuffer *fb, int x, int y, const char *text, unsigned scale,
              uint32_t color) {
    if (!gfx_framebuffer_valid(fb) || !text || !scale || scale > (unsigned)INT_MAX / 8u) return;
    int64_t pen_x = x, pen_y = y;
    while (*text) {
        unsigned char c = (unsigned char)*text++;
        if (c == '\n') {
            pen_x = x;
            pen_y += 8 * (int64_t)scale;
            if (pen_y > INT_MAX) return;
            continue;
        }
        /* A glyph wholly outside the surface (or the clipped view being composed)
         * draws nothing; skip its per-cell clipping work. */
        bool visible_glyph = pen_x < (int64_t)fb->width && pen_y < (int64_t)fb->height &&
                             pen_x + 5 * (int64_t)scale > 0 && pen_y + 7 * (int64_t)scale > 0;
        for (unsigned row = 0; visible_glyph && row < 7; ++row) {
            uint8_t bits = glyph_row(c, row);
            for (unsigned col = 0; col < 5; ++col) {
                if (!(bits & (1u << (4u - col)))) continue;
                int64_t px = pen_x + col * (int64_t)scale;
                int64_t py = pen_y + row * (int64_t)scale;
                if (px >= INT_MIN && px <= INT_MAX && py >= INT_MIN && py <= INT_MAX)
                    gfx_rect(fb, (int)px, (int)py, (int)scale, (int)scale, color);
            }
        }
        /* Saturate offscreen horizontal text, while still honoring newlines. */
        if (pen_x <= INT_MAX) pen_x += 6 * (int64_t)scale;
    }
}

int gfx_text_width(const char *text, unsigned scale) {
    if (!text || !scale || scale > (unsigned)INT_MAX / 8u) return 0;
    uint64_t widest = 0, line = 0;
    for (;; ++text) {
        if (*text && *text != '\n') {
            ++line;
            continue;
        }
        if (line > widest) widest = line;
        line = 0;
        if (!*text) break;
    }
    uint64_t width = widest ? (widest * 6 - 1) * scale : 0;
    return width > INT_MAX ? INT_MAX : (int)width;
}

bool gfx_present(struct framebuffer *visible, const struct framebuffer *completed) {
    if (!gfx_framebuffer_valid(visible) || !gfx_framebuffer_valid(completed) ||
        visible->width != completed->width || visible->height != completed->height ||
        visible->format != completed->format)
        return false;
    uintptr_t dst_start = (uintptr_t)visible->pixels, src_start = (uintptr_t)completed->pixels;
    size_t dst_size = (size_t)visible->stride * visible->height;
    size_t src_size = (size_t)completed->stride * completed->height;
    if (dst_start < src_start + src_size && src_start < dst_start + dst_size) return false;
    /* Copy the geometry: stores through the scanout pointers may alias *visible,
     * which would otherwise force a reload of its width for every pixel. */
    const unsigned bytes = bytes_per_pixel(visible->format);
    const uint32_t width = visible->width, height = visible->height;
    const size_t dst_stride = visible->stride, src_stride = completed->stride;
    volatile uint8_t *dst = (volatile uint8_t *)visible->pixels;
    const uint8_t *src = (const uint8_t *)completed->pixels;
    /* Volatile scanout accesses retain the conditional stores: an unchanged
     * title/button must never be cleared and drawn again. */
    for (uint32_t y = 0; y < height; ++y, dst += dst_stride, src += src_stride) {
        if (bytes == 4) {
            volatile uint32_t *to = (volatile uint32_t *)dst;
            const uint32_t *from = (const uint32_t *)src;
            for (uint32_t x = 0; x < width; ++x)
                if (to[x] != from[x]) to[x] = from[x];
        } else if (bytes == 2) {
            volatile uint16_t *to = (volatile uint16_t *)dst;
            const uint16_t *from = (const uint16_t *)src;
            for (uint32_t x = 0; x < width; ++x)
                if (to[x] != from[x]) to[x] = from[x];
        } else {
            for (size_t i = 0; i < (size_t)width * 3; ++i)
                if (dst[i] != src[i]) dst[i] = src[i];
        }
    }
    return true;
}

/* The largest scanout: the Nexus 5's 1080x1920 panel on ARM32 (the Redmi 2's
 * is 720x1280), or on AArch64 the Galaxy Tab A7 Lite's 800x1340. */
#ifdef __aarch64__
enum { SCANOUT_WIDTH = 800, SCANOUT_HEIGHT = 1340 };
#else
enum { SCANOUT_WIDTH = 1080, SCANOUT_HEIGHT = 1920 };
#endif

bool gfx_present_scaled_region(struct framebuffer *visible, const struct framebuffer *source,
                               uint32_t left, uint32_t top, uint32_t width, uint32_t height) {
    if (!gfx_framebuffer_valid(visible) || !gfx_framebuffer_valid(source) ||
        source->format != PIXEL_XRGB8888 || visible->width > SCANOUT_WIDTH ||
        visible->height > SCANOUT_HEIGHT)
        return false;
    uintptr_t a = (uintptr_t)visible->pixels, b = (uintptr_t)source->pixels;
    if (a < b + (size_t)source->stride * source->height &&
        b < a + (size_t)visible->stride * visible->height)
        return false;
    if (left >= source->width || top >= source->height || !width || !height) return true;
    if (width > source->width - left) width = source->width - left;
    if (height > source->height - top) height = source->height - top;
    /* Copy geometry before writing scanout; stores must not force an uncached
     * reload of framebuffer metadata in every iteration. */
    const uint32_t vw = visible->width, vh = visible->height, sw = source->width,
                   sh = source->height;
    const size_t vs = visible->stride, ss = source->stride;
    const enum pixel_format format = visible->format;
    const unsigned bytes = bytes_per_pixel(format);
    const unsigned char *src = source->pixels;
    volatile unsigned char *dst = visible->pixels;
    if (!left && !top && width == sw && height == sh && vw == sw && vh == sh &&
        format == source->format)
        return gfx_present(visible, source);
    /* ceil(source_edge * destination_extent / source_extent) preserves the
     * full-frame sampling phase for non-integer scaling and clipped regions. */
    unsigned x0 = ((uint64_t)left * vw + sw - 1) / sw,
             x1 = ((uint64_t)(left + width) * vw + sw - 1) / sw;
    unsigned y0 = ((uint64_t)top * vh + sh - 1) / sh,
             y1 = ((uint64_t)(top + height) * vh + sh - 1) / sh;
    unsigned columns[SCANOUT_WIDTH];
    for (unsigned x = x0; x < x1; ++x) columns[x] = (uint64_t)x * sw / vw;
    for (unsigned y = y0; y < y1; ++y) {
        const uint32_t *row = (const uint32_t *)(src + (size_t)((uint64_t)y * sh / vh) * ss);
        volatile unsigned char *out = dst + (size_t)y * vs;
        /* Damage is already known. Reading uncached scanout to compare every
         * pixel costs more than publishing it, especially with packed RGB888. */
        if (bytes == 2) {
            volatile uint16_t *to = (volatile uint16_t *)out;
            for (unsigned x = x0; x < x1; ++x) {
                uint32_t value = row[columns[x]];
                uint16_t pixel =
                    ((value >> 8) & 0xf800) | ((value >> 5) & 0x7e0) | ((value >> 3) & 0x1f);
                to[x] = pixel;
            }
        } else if (bytes == 4) {
            volatile uint32_t *to = (volatile uint32_t *)out;
            uint32_t alpha = format == PIXEL_ARGB8888 ? 0xff000000 : 0;
            for (unsigned x = x0; x < x1; ++x) to[x] = row[columns[x]] | alpha;
        } else {
            unsigned x = x0;
            volatile unsigned char *to = out + x * 3;
            while (x < x1 && ((uintptr_t)to & 3)) {
                uint32_t value = row[columns[x++]];
                *to++ = value;
                *to++ = value >> 8;
                *to++ = value >> 16;
            }
            /* Four packed pixels occupy three aligned words. No padding or
             * neighboring damage pixels are read or overwritten. */
            volatile uint32_t *words = (volatile uint32_t *)to;
            while (x1 - x >= 4) {
                uint32_t a = row[columns[x]], b = row[columns[x + 1]], c = row[columns[x + 2]],
                         d = row[columns[x + 3]];
                *words++ = (a & 0xffffff) | (b << 24);
                *words++ = ((b >> 8) & 0xffff) | (c << 16);
                *words++ = ((c >> 16) & 0xff) | (d << 8);
                x += 4;
            }
            to = (volatile unsigned char *)words;
            while (x < x1) {
                uint32_t value = row[columns[x++]];
                *to++ = value;
                *to++ = value >> 8;
                *to++ = value >> 16;
            }
        }
    }
    return true;
}

bool gfx_present_scaled(struct framebuffer *visible, const struct framebuffer *source) {
    return source &&
           gfx_present_scaled_region(visible, source, 0, 0, source->width, source->height);
}
