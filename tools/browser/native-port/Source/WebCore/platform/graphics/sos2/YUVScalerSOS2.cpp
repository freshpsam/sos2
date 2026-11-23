#include "config.h"
#include "YUVScalerSOS2.h"

#include <algorithm>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define SOS2_YUV_NEON 1
#endif

namespace WebCore {

namespace {
constexpr unsigned groupBytes = 32;
constexpr uint16_t byPixel = 0xffff;
}

// Output pixel x samples the source at (x + 0.5) * from / to - 0.5, in 16.16
// fixed point; its left pixel and the right one's weight in 128ths.
void YUVScalerSOS2::prepare(Line& line, int from, int to)
{
    if (line.from == from && line.to == to)
        return;
    line.from = from;
    line.to = to;
    line.left.resize(to);
    line.weight.resize(to);
    for (int x = 0; x < to; ++x) {
        int64_t position = ((2 * int64_t(x) + 1) * from * 65536) / (2 * int64_t(to)) - 32768;
        position = std::max<int64_t>(position, 0);
        int left = int(position >> 16), weight = int((position >> 9) & 127);
        if (left >= from - 1) {
            left = from - 1;
            weight = 0;
        }
        line.left[x] = left;
        line.weight[x] = weight;
    }
    unsigned groups = to / 8;
    line.groups.resize(groups * groupBytes);
    line.groupStart.resize(groups);
    for (unsigned g = 0; g < groups; ++g) {
        int start = line.left[g * 8];
        bool fits = start + 24 <= from;
        uint8_t* entry = &line.groups[g * groupBytes];
        for (unsigned k = 0; k < 8 && fits; ++k) {
            unsigned x = g * 8 + k;
            int left = line.left[x] - start, right = std::min(line.left[x] + 1, from - 1) - start;
            fits = right < 24;
            entry[k] = left;
            entry[8 + k] = right;
            entry[16 + k] = 128 - line.weight[x];
            entry[24 + k] = line.weight[x];
        }
        line.groupStart[g] = fits ? start : byPixel;
    }
}

// A source row across, into values in 128ths (0-32640).
void YUVScalerSOS2::scaleRow(const Line& line, const uint8_t* source, uint16_t* out)
{
    auto pixel = [&](unsigned x) {
        unsigned left = line.left[x], right = std::min<unsigned>(left + 1, line.from - 1), weight = line.weight[x];
        out[x] = source[left] * (128 - weight) + source[right] * weight;
    };
    unsigned x = 0;
#if SOS2_YUV_NEON
    for (unsigned g = 0; g < line.groupStart.size(); ++g, x += 8) {
        unsigned start = line.groupStart[g];
        if (start == byPixel) {
            for (unsigned k = 0; k < 8; ++k)
                pixel(x + k);
            continue;
        }
        const uint8_t* entry = &line.groups[g * groupBytes];
        const uint8_t* window = source + start;
        uint8x8x3_t table = { { vld1_u8(window), vld1_u8(window + 8), vld1_u8(window + 16) } };
        uint8x8_t left = vtbl3_u8(table, vld1_u8(entry));
        uint8x8_t right = vtbl3_u8(table, vld1_u8(entry + 8));
        uint16x8_t value = vmull_u8(left, vld1_u8(entry + 16));
        vst1q_u16(out + x, vmlal_u8(value, right, vld1_u8(entry + 24)));
    }
#endif
    for (; x < unsigned(line.to); ++x)
        pixel(x);
}

// Each output row blends the two source rows around it, scaled across; the
// two scaled last are kept, as the next output row mostly needs them again.
void YUVScalerSOS2::scalePlane(const uint8_t* source, int sourceStride, IntSize from, uint8_t* destination,
    int destinationStride, IntSize to, Line& across, Line& down)
{
    prepare(across, from.width(), to.width());
    prepare(down, from.height(), to.height());
    for (auto& row : m_rows) {
        if (row.size() < unsigned(to.width()))
            row.resize(to.width());
    }
    m_rowSource[0] = m_rowSource[1] = -1;
    auto find = [&](int row) {
        return m_rowSource[0] == row ? 0 : m_rowSource[1] == row ? 1 : -1;
    };
    auto fill = [&](int slot, int row) {
        scaleRow(across, source + ptrdiff_t(row) * sourceStride, m_rows[slot].mutableSpan().data());
        m_rowSource[slot] = row;
    };
    for (int y = 0; y < to.height(); ++y) {
        int top = down.left[y], bottom = std::min(top + 1, from.height() - 1);
        int upper = find(top);
        if (upper < 0) {
            upper = find(bottom) == 0 ? 1 : 0;
            fill(upper, top);
        }
        int lower = find(bottom);
        if (lower < 0) {
            lower = upper ^ 1;
            fill(lower, bottom);
        }
        const uint16_t* a = m_rows[upper].span().data();
        const uint16_t* b = m_rows[lower].span().data();
        uint8_t* out = destination + ptrdiff_t(y) * destinationStride;
        unsigned wa = 128 - down.weight[y], wb = down.weight[y];
        int x = 0;
#if SOS2_YUV_NEON
        for (; x + 8 <= to.width(); x += 8) {
            uint16x8_t ua = vld1q_u16(a + x), ub = vld1q_u16(b + x);
            uint32x4_t low = vmlal_n_u16(vmull_n_u16(vget_low_u16(ua), wa), vget_low_u16(ub), wb);
            uint32x4_t high = vmlal_n_u16(vmull_n_u16(vget_high_u16(ua), wa), vget_high_u16(ub), wb);
            vst1_u8(out + x, vmovn_u16(vcombine_u16(vrshrn_n_u32(low, 14), vrshrn_n_u32(high, 14))));
        }
#endif
        for (; x < to.width(); ++x)
            out[x] = (a[x] * wa + b[x] * wb + 8192) >> 14;
    }
}

void YUVScalerSOS2::scale(const uint8_t* const source[3], const int sourceStrides[3], IntSize from,
    uint8_t* const destination[3], const int destinationStrides[3], IntSize to)
{
    if (from.isEmpty() || to.isEmpty())
        return;
    scalePlane(source[0], sourceStrides[0], from, destination[0], destinationStrides[0], to, m_across[0], m_down[0]);
    IntSize fromChroma((from.width() + 1) / 2, (from.height() + 1) / 2);
    IntSize toChroma((to.width() + 1) / 2, (to.height() + 1) / 2);
    for (int plane = 1; plane < 3; ++plane)
        scalePlane(source[plane], sourceStrides[plane], fromChroma, destination[plane], destinationStrides[plane], toChroma, m_across[1], m_down[1]);
}

} // namespace WebCore
