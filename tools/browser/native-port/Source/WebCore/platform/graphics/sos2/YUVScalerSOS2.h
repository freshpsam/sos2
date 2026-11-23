#pragma once

#include "IntSize.h"
#include <array>
#include <cstdint>
#include <wtf/Vector.h>

namespace WebCore {

// Scales 4:2:0 planes, bilinear with 7-bit weights as FFmpeg's fast bilinear
// scaler, but eight pixels at a time in NEON: across, each group of eight
// takes its two neighbours from the 24 source bytes around it through table
// lookups; down, two scaled rows are blended. FFmpeg ran that scaler in C,
// about 9 ms a 480p frame on the Nexus 5, and its NEON one wants filters of
// eight taps. Frames sample pixel centres, as the GPU would. Groups whose
// sources span more than 24 bytes (from frames over three times wider than
// shown) and the ends of rows go a pixel at a time.
class YUVScalerSOS2 {
public:
    void scale(const uint8_t* const source[3], const int sourceStrides[3], IntSize from,
        uint8_t* const destination[3], const int destinationStrides[3], IntSize to);

private:
    // Where each output pixel of a line samples: the left source pixel and
    // the right one's weight (0-128), then each NEON group's table lookups.
    struct Line {
        int from { 0 }, to { 0 };
        Vector<uint16_t> left;
        Vector<uint8_t> weight;
        Vector<uint8_t> groups; // 32 bytes a group: left, right, left weight, right weight
        Vector<uint16_t> groupStart; // its first source byte, or 0xffff a pixel at a time
    };

    static void prepare(Line&, int from, int to);
    static void scaleRow(const Line&, const uint8_t* source, uint16_t* out);
    void scalePlane(const uint8_t* source, int sourceStride, IntSize from, uint8_t* destination,
        int destinationStride, IntSize to, Line& across, Line& down);

    std::array<Line, 2> m_across; // luma, chroma
    std::array<Line, 2> m_down;
    Vector<uint16_t> m_rows[2];
    int m_rowSource[2] { -1, -1 };
};

} // namespace WebCore
