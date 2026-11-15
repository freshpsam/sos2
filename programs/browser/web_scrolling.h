#pragma once
#include "web_damage.h"

namespace SOS2 {
// The pixels of a scroll whose source and destination both lie inside the
// scroll rectangle: their destination, or nothing if no pixel stays inside.
inline PixelDamage scrollDestination(unsigned stride, unsigned height, PixelDamage rect, int dx,
                                     int dy) {
    if (!rect.width || !rect.height || rect.x >= stride || rect.y >= height ||
        rect.width > stride - rect.x || rect.height > height - rect.y ||
        int64_t(dx) >= rect.width || int64_t(dx) <= -int64_t(rect.width) ||
        int64_t(dy) >= rect.height || int64_t(dy) <= -int64_t(rect.height))
        return {};
    unsigned ax = dx < 0 ? unsigned(-dx) : unsigned(dx), ay = dy < 0 ? unsigned(-dy) : unsigned(dy);
    return {rect.x + (dx > 0 ? ax : 0), rect.y + (dy > 0 ? ay : 0), rect.width - ax,
            rect.height - ay};
}

// Move only pixels whose source and destination both lie inside the scroll
// rectangle. The caller repaints rect minus the returned destination. Row
// order plus memmove handles overlap in either axis without a scratch image.
inline PixelDamage scrollPixels(uint32_t *pixels, unsigned stride, unsigned height,
                                PixelDamage rect, int dx, int dy) {
    auto destination = pixels ? scrollDestination(stride, height, rect, dx, dy) : PixelDamage{};
    if (!destination.width) return {};
    unsigned sx = destination.x - (dx > 0 ? unsigned(dx) : 0) + (dx < 0 ? unsigned(-dx) : 0);
    unsigned sy = destination.y - (dy > 0 ? unsigned(dy) : 0) + (dy < 0 ? unsigned(-dy) : 0);
    if (dy && destination.width == stride) {
        // Full-width vertical scrolling is one contiguous overlapping move.
        std::memmove(pixels + destination.y * stride, pixels + sy * stride,
                     size_t(destination.height) * stride * sizeof(*pixels));
    } else if (dx || dy)
        for (unsigned i = 0; i < destination.height; ++i) {
            unsigned row = dy > 0 ? destination.height - 1 - i : i;
            std::memmove(pixels + (destination.y + row) * stride + destination.x,
                         pixels + (sy + row) * stride + sx, destination.width * sizeof(*pixels));
        }
    return destination;
}
}
