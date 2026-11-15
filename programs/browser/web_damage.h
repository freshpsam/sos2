#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace SOS2 {
struct PixelDamage {
    unsigned x, y, width, height;
};

struct PixelDamageList {
    static constexpr unsigned limit = 12;
    PixelDamage rects[limit + 1]{};
    unsigned count = 0;
};

// Whether damage of `count` rectangles covering `area` pixels is drawn (or read
// back) as its bounds of `boundsArea` pixels instead: every rectangle costs a
// render-tree walk or a device round trip, so more than 12 rectangles, or
// rectangles filling at least half their bounds, go as one. byDensity=false
// applies only the count.
inline bool coalesceDamage(size_t count, uint64_t area, uint64_t boundsArea,
                           bool byDensity = true) {
    return count > 12 || (byDensity && area * 2 >= boundsArea);
}

inline PixelDamage unite(PixelDamage a, PixelDamage b) {
    unsigned x = a.x < b.x ? a.x : b.x, y = a.y < b.y ? a.y : b.y;
    unsigned right = a.x + a.width > b.x + b.width ? a.x + a.width : b.x + b.width;
    unsigned bottom = a.y + a.height > b.y + b.height ? a.y + a.height : b.y + b.height;
    return {x, y, right - x, bottom - y};
}

// Compare cached renderer memory, never the uncached physical framebuffer.
// Bounds come from the clipped WebKit paint rectangle. Exact comparisons avoid
// hash collisions; focus/visibility invalidations can repaint identical pixels.
inline PixelDamage changedPixels(const uint32_t *current, const uint32_t *presented,
                                 unsigned stride, PixelDamage paint) {
    PixelDamage result{};
    for (unsigned y = paint.y; y < paint.y + paint.height; ++y) {
        const auto *a = current + y * stride;
        const auto *b = presented + y * stride;
        if (!std::memcmp(a + paint.x, b + paint.x, paint.width * sizeof(*a))) continue;
        unsigned left = paint.x, right = paint.x + paint.width;
        while (a[left] == b[left]) ++left;
        while (a[right - 1] == b[right - 1]) --right;
        if (!result.width) result = {left, y, right - left, 1};
        else {
            unsigned end = result.x + result.width;
            if (right > end) end = right;
            if (left < result.x) result.x = left;
            result.width = end - result.x;
            result.height = y + 1 - result.y;
        }
    }
    return result;
}

// Scan in short bands so a scrollbar and an exposed strip do not force one
// full-viewport upload. Coalesce equal spans; if the fixed budget fills, merge
// the least expensive pair. Every changed pixel remains covered, without hashes
// or heap allocation. A small isolated repaint retains its exact pixel bounds.
inline void appendChangedRegions(PixelDamageList &result, const uint32_t *current,
                                 const uint32_t *presented, unsigned stride, PixelDamage paint) {
    for (unsigned y = paint.y; y < paint.y + paint.height;) {
        unsigned height = paint.y + paint.height - y;
        if (height > 32) height = 32;
        auto r = changedPixels(current, presented, stride, {paint.x, y, paint.width, height});
        y += height;
        if (!r.width) continue;
        if (result.count) {
            auto &last = result.rects[result.count - 1];
            if (last.x == r.x && last.width == r.width && last.y + last.height == r.y) {
                last.height += r.height;
                continue;
            }
        }
        result.rects[result.count++] = r;
        if (result.count <= PixelDamageList::limit) continue;
        unsigned a = 0, b = 1;
        int64_t cost = INT64_MAX;
        for (unsigned i = 0; i < result.count; ++i)
            for (unsigned j = i + 1; j < result.count; ++j) {
                auto merged = unite(result.rects[i], result.rects[j]);
                int64_t extra = int64_t(merged.width) * merged.height -
                                int64_t(result.rects[i].width) * result.rects[i].height -
                                int64_t(result.rects[j].width) * result.rects[j].height;
                if (extra < cost) {
                    a = i;
                    b = j;
                    cost = extra;
                }
            }
        result.rects[a] = unite(result.rects[a], result.rects[b]);
        for (unsigned i = b + 1; i < result.count; ++i) result.rects[i - 1] = result.rects[i];
        --result.count;
    }
}

inline PixelDamageList changedRegions(const uint32_t *current, const uint32_t *presented,
                                      unsigned stride, PixelDamage paint) {
    PixelDamageList result;
    appendChangedRegions(result, current, presented, stride, paint);
    return result;
}
}
