#pragma once

#if ENABLE(MEDIA_SOURCE)

#include <span>
#include <wtf/Function.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

// Fragmented MP4 (ISO/IEC 14496-12) as Media Source Extensions appends it: an
// initialization segment (moov) and media segments (moof, then mdat), parsed
// from whole top-level boxes as they arrive. Tracks are H.264 (avc1, avc3) or
// AAC (mp4a); others are left out of the initialization segment.
class ISOBMFFParserSOS2 {
public:
    enum class Kind : uint8_t { Video, Audio };

    struct Track {
        uint32_t id { 0 };
        Kind kind { Kind::Video };
        String codec; // RFC 6381: avc1.PPCCLL or mp4a.40.N
        uint32_t timescale { 0 };
        uint32_t width { 0 };
        uint32_t height { 0 };
        uint32_t sampleRate { 0 };
        uint32_t channels { 0 };
        Vector<uint8_t> extradata; // the avcC record, or the AudioSpecificConfig
        int64_t presentationStart { 0 }; // the first edit's media time, in timescale units
        uint32_t defaultDuration { 0 };
        uint32_t defaultSize { 0 };
        uint32_t defaultFlags { 0 };
        uint64_t nextDecodeTime { 0 }; // for a fragment without tfdt
    };

    struct Initialization {
        double duration { 0 }; // seconds; 0 when the segment leaves it open
        Vector<Track> tracks;
    };

    struct Sample {
        uint32_t trackID { 0 };
        uint32_t timescale { 0 };
        int64_t decodeTime { 0 };
        int64_t presentationTime { 0 };
        uint32_t duration { 0 };
        bool sync { false };
        Vector<uint8_t> data;
    };

    // False on data that is not fragmented MP4.
    bool append(std::span<const uint8_t>, NOESCAPE const Function<void(Initialization&&)>&, NOESCAPE const Function<void(Sample&&)>&);
    // Drops a partly received box and the runs of an unfinished media segment.
    void reset();

private:
    struct Run {
        uint32_t trackID;
        uint64_t offset; // in the appended stream
        uint32_t size;
        int64_t decodeTime;
        int64_t compositionOffset;
        uint32_t duration;
        bool sync;
    };

    bool parseInitialization(std::span<const uint8_t>, NOESCAPE const Function<void(Initialization&&)>&);
    bool parseFragment(std::span<const uint8_t>, uint64_t offset);
    bool parseMediaData(std::span<const uint8_t> payload, uint64_t payloadOffset, NOESCAPE const Function<void(Sample&&)>&);
    Track* track(uint32_t id);

    Vector<uint8_t> m_input;
    uint64_t m_inputOffset { 0 }; // the stream offset of m_input's first byte
    Vector<Track> m_tracks;
    Vector<Run> m_runs;
};

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
