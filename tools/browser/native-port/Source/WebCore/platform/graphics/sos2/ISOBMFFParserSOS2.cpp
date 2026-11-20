#include "config.h"
#include "ISOBMFFParserSOS2.h"

#if ENABLE(MEDIA_SOURCE)

#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>

namespace WebCore {

namespace {

constexpr uint32_t fourCC(const char (&code)[5])
{
    return uint32_t(uint8_t(code[0])) << 24 | uint32_t(uint8_t(code[1])) << 16 | uint32_t(uint8_t(code[2])) << 8 | uint8_t(code[3]);
}

// A box larger than this is not waited for: no media segment comes close.
constexpr uint64_t maximumBoxSize = 256 << 20;
// Samples a media segment may list (12 MiB of runs): a run without per-sample
// fields reads nothing per sample, so its count, which the page supplies,
// would otherwise be the only bound.
constexpr size_t maximumRuns = 1 << 18;

class Reader {
public:
    explicit Reader(std::span<const uint8_t> data)
        : m_data(data)
    {
    }

    bool failed() const { return m_failed; }
    size_t remaining() const { return m_data.size() - m_position; }
    uint8_t u8() { return static_cast<uint8_t>(read(1)); }
    uint16_t u16() { return static_cast<uint16_t>(read(2)); }
    uint32_t u24() { return static_cast<uint32_t>(read(3)); }
    uint32_t u32() { return static_cast<uint32_t>(read(4)); }
    uint64_t u64() { return read(8); }

    void skip(size_t count)
    {
        if (count > remaining()) {
            fail();
            return;
        }
        m_position += count;
    }

    std::span<const uint8_t> bytes(size_t count)
    {
        if (count > remaining()) {
            fail();
            return { };
        }
        auto result = m_data.subspan(m_position, count);
        m_position += count;
        return result;
    }

    std::span<const uint8_t> rest() { return bytes(remaining()); }

private:
    uint64_t read(unsigned count)
    {
        if (count > remaining()) {
            fail();
            return 0;
        }
        uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i)
            value = value << 8 | m_data[m_position + i];
        m_position += count;
        return value;
    }

    void fail()
    {
        m_failed = true;
        m_position = m_data.size();
    }

    std::span<const uint8_t> m_data;
    size_t m_position { 0 };
    bool m_failed { false };
};

struct BoxHeader {
    uint32_t type;
    uint64_t size;
    unsigned headerSize;
};

// The header at the start of data once all of it has arrived. A size of 0
// (the box runs to the end of the file) is left at 0, which callers reject.
std::optional<BoxHeader> boxHeader(std::span<const uint8_t> data)
{
    if (data.size() < 8)
        return std::nullopt;
    Reader reader(data);
    uint64_t size = reader.u32();
    uint32_t type = reader.u32();
    unsigned headerSize = 8;
    if (size == 1) {
        if (data.size() < 16)
            return std::nullopt;
        size = reader.u64();
        headerSize = 16;
    }
    return BoxHeader { type, size, headerSize };
}

// Calls visit(type, payload) for each box in data; false on a malformed box
// or when visit returns false.
template<typename Visit>
bool forEachBox(std::span<const uint8_t> data, Visit&& visit)
{
    while (!data.empty()) {
        auto header = boxHeader(data);
        if (!header || header->size < header->headerSize || header->size > data.size())
            return false;
        if (!visit(header->type, data.subspan(header->headerSize, header->size - header->headerSize)))
            return false;
        data = data.subspan(header->size);
    }
    return true;
}

// The AudioSpecificConfig in an esds box's MPEG-4 audio decoder descriptor.
std::optional<Vector<uint8_t>> audioSpecificConfig(std::span<const uint8_t> esds)
{
    Reader reader(esds);
    reader.skip(4);
    auto descriptor = [&](uint8_t expected) -> std::optional<size_t> {
        if (reader.u8() != expected)
            return std::nullopt;
        size_t size = 0;
        for (unsigned i = 0; i < 4; ++i) {
            uint8_t byte = reader.u8();
            size = size << 7 | (byte & 0x7f);
            if (!(byte & 0x80))
                break;
        }
        return reader.failed() ? std::nullopt : std::optional { size };
    };
    if (!descriptor(3))
        return std::nullopt;
    reader.skip(2);
    uint8_t flags = reader.u8();
    if (flags & 0x80)
        reader.skip(2);
    if (flags & 0x40)
        reader.skip(reader.u8());
    if (flags & 0x20)
        reader.skip(2);
    if (!descriptor(4) || reader.u8() != 0x40) // MPEG-4 audio
        return std::nullopt;
    reader.skip(12);
    auto size = descriptor(5);
    if (!size || !*size)
        return std::nullopt;
    auto config = reader.bytes(*size);
    if (reader.failed())
        return std::nullopt;
    return Vector<uint8_t>(config);
}

// stsd's first sample entry, if it is H.264 or AAC.
bool parseSampleEntry(std::span<const uint8_t> stsd, ISOBMFFParserSOS2::Track& track)
{
    Reader reader(stsd);
    reader.skip(8); // version, flags and entry count
    return forEachBox(reader.rest(), [&](uint32_t type, std::span<const uint8_t> entry) {
        if (!track.codec.isEmpty())
            return true;
        if (type == fourCC("avc1") || type == fourCC("avc3")) {
            if (entry.size() < 78)
                return false;
            Reader visual(entry);
            visual.skip(24);
            track.width = visual.u16();
            track.height = visual.u16();
            return forEachBox(entry.subspan(78), [&](uint32_t type, std::span<const uint8_t> payload) {
                if (type != fourCC("avcC") || payload.size() < 7)
                    return true;
                track.kind = ISOBMFFParserSOS2::Kind::Video;
                track.extradata = Vector<uint8_t>(payload);
                // avcC's profile, compatibility and level bytes (avc3 decodes alike).
                track.codec = makeString("avc1."_s, hex(payload[1], 2, Lowercase), hex(payload[2], 2, Lowercase), hex(payload[3], 2, Lowercase));
                return true;
            });
        }
        if (type == fourCC("mp4a")) {
            if (entry.size() < 28)
                return false;
            Reader audio(entry);
            audio.skip(16);
            track.channels = audio.u16();
            audio.skip(6);
            track.sampleRate = audio.u32() >> 16;
            return forEachBox(entry.subspan(28), [&](uint32_t type, std::span<const uint8_t> payload) {
                if (type != fourCC("esds"))
                    return true;
                auto config = audioSpecificConfig(payload);
                if (!config || config->size() < 2)
                    return true;
                unsigned objectType = (*config)[0] >> 3;
                if (objectType == 31)
                    objectType = 32 + (((*config)[0] & 7) << 3 | (*config)[1] >> 5);
                track.kind = ISOBMFFParserSOS2::Kind::Audio;
                track.extradata = WTF::move(*config);
                track.codec = makeString("mp4a.40."_s, objectType);
                return true;
            });
        }
        return true;
    });
}

bool parseTrack(std::span<const uint8_t> trak, ISOBMFFParserSOS2::Track& track)
{
    uint32_t handler = 0;
    bool parsed = forEachBox(trak, [&](uint32_t type, std::span<const uint8_t> payload) {
        if (type == fourCC("tkhd")) {
            Reader reader(payload);
            uint8_t version = reader.u8();
            reader.skip(version == 1 ? 19 : 11);
            track.id = reader.u32();
            return !reader.failed();
        }
        if (type == fourCC("edts")) {
            return forEachBox(payload, [&](uint32_t type, std::span<const uint8_t> payload) {
                if (type != fourCC("elst"))
                    return true;
                Reader reader(payload);
                uint8_t version = reader.u8();
                reader.skip(3);
                for (uint32_t count = reader.u32(); count && !reader.failed(); --count) {
                    reader.skip(version == 1 ? 8 : 4);
                    int64_t mediaTime = version == 1 ? static_cast<int64_t>(reader.u64()) : static_cast<int32_t>(reader.u32());
                    reader.skip(4);
                    if (mediaTime >= 0) {
                        track.presentationStart = mediaTime;
                        break;
                    }
                }
                return !reader.failed();
            });
        }
        if (type != fourCC("mdia"))
            return true;
        return forEachBox(payload, [&](uint32_t type, std::span<const uint8_t> payload) {
            Reader reader(payload);
            if (type == fourCC("mdhd")) {
                uint8_t version = reader.u8();
                reader.skip(version == 1 ? 19 : 11);
                track.timescale = reader.u32();
                return !reader.failed();
            }
            if (type == fourCC("hdlr")) {
                reader.skip(8);
                handler = reader.u32();
                return !reader.failed();
            }
            if (type != fourCC("minf"))
                return true;
            return forEachBox(payload, [&](uint32_t type, std::span<const uint8_t> payload) {
                if (type != fourCC("stbl"))
                    return true;
                return forEachBox(payload, [&](uint32_t type, std::span<const uint8_t> payload) {
                    return type != fourCC("stsd") || parseSampleEntry(payload, track);
                });
            });
        });
    });
    if (!parsed)
        return false;
    // A sample entry for one kind of media in another's track, or no time
    // scale, leaves the track out.
    if ((track.kind == ISOBMFFParserSOS2::Kind::Video) != (handler == fourCC("vide")) || !track.timescale)
        track.codec = { };
    return true;
}

} // namespace

bool ISOBMFFParserSOS2::append(std::span<const uint8_t> data, NOESCAPE const Function<void(Initialization&&)>& initialization, NOESCAPE const Function<void(Sample&&)>& sample)
{
    m_input.append(data);
    size_t position = 0;
    bool valid = true;
    while (valid) {
        auto available = m_input.span().subspan(position);
        auto header = boxHeader(available);
        if (!header)
            break;
        if (header->size < header->headerSize || header->size > maximumBoxSize) {
            valid = false;
            break;
        }
        if (header->size > available.size())
            break;
        auto payload = available.subspan(header->headerSize, header->size - header->headerSize);
        uint64_t offset = m_inputOffset + position;
        switch (header->type) {
        case fourCC("moov"):
            valid = parseInitialization(payload, initialization);
            break;
        case fourCC("moof"):
            valid = parseFragment(payload, offset);
            break;
        case fourCC("mdat"):
            valid = parseMediaData(payload, offset + header->headerSize, sample);
            break;
        default: // ftyp, styp, sidx, emsg, prft, free, skip
            break;
        }
        position += header->size;
    }
    m_input.removeAt(0, position);
    m_inputOffset += position;
    if (!valid)
        reset();
    return valid;
}

void ISOBMFFParserSOS2::reset()
{
    m_inputOffset += m_input.size();
    m_input.clear();
    m_runs.clear();
}

ISOBMFFParserSOS2::Track* ISOBMFFParserSOS2::track(uint32_t id)
{
    for (auto& track : m_tracks) {
        if (track.id == id)
            return &track;
    }
    return nullptr;
}

bool ISOBMFFParserSOS2::parseInitialization(std::span<const uint8_t> moov, NOESCAPE const Function<void(Initialization&&)>& callback)
{
    struct Defaults {
        uint32_t id;
        uint32_t duration;
        uint32_t size;
        uint32_t flags;
    };
    Vector<Track> tracks;
    Vector<Defaults> defaults;
    uint32_t movieTimescale = 0;
    uint64_t movieDuration = 0;
    uint64_t fragmentDuration = 0;
    bool parsed = forEachBox(moov, [&](uint32_t type, std::span<const uint8_t> payload) {
        Reader reader(payload);
        if (type == fourCC("mvhd")) {
            uint8_t version = reader.u8();
            reader.skip(version == 1 ? 19 : 11);
            movieTimescale = reader.u32();
            movieDuration = version == 1 ? reader.u64() : reader.u32();
            return !reader.failed();
        }
        if (type == fourCC("trak")) {
            Track track;
            if (!parseTrack(payload, track))
                return false;
            if (!track.codec.isEmpty())
                tracks.append(WTF::move(track));
            return true;
        }
        if (type != fourCC("mvex"))
            return true;
        return forEachBox(payload, [&](uint32_t type, std::span<const uint8_t> payload) {
            Reader reader(payload);
            if (type == fourCC("mehd")) {
                uint8_t version = reader.u8();
                reader.skip(3);
                fragmentDuration = version == 1 ? reader.u64() : reader.u32();
            } else if (type == fourCC("trex")) {
                reader.skip(4);
                uint32_t id = reader.u32();
                reader.skip(4);
                uint32_t duration = reader.u32();
                uint32_t size = reader.u32();
                uint32_t flags = reader.u32();
                defaults.append({ id, duration, size, flags });
            }
            return !reader.failed();
        });
    });
    if (!parsed || !movieTimescale)
        return false;

    for (auto& track : tracks) {
        for (auto& entry : defaults) {
            if (entry.id == track.id) {
                track.defaultDuration = entry.duration;
                track.defaultSize = entry.size;
                track.defaultFlags = entry.flags;
            }
        }
        // A repeated initialization segment keeps each track's position.
        if (auto* previous = this->track(track.id))
            track.nextDecodeTime = previous->nextDecodeTime;
    }
    m_tracks = tracks;
    uint64_t duration = fragmentDuration ? fragmentDuration : movieDuration;
    callback({ duration && duration != UINT32_MAX && duration != UINT64_MAX ? static_cast<double>(duration) / movieTimescale : 0, WTF::move(tracks) });
    return true;
}

bool ISOBMFFParserSOS2::parseFragment(std::span<const uint8_t> moof, uint64_t moofOffset)
{
    m_runs.clear();
    uint64_t previousEnd = moofOffset;
    bool firstFragment = true;
    return forEachBox(moof, [&](uint32_t type, std::span<const uint8_t> traf) {
        if (type != fourCC("traf"))
            return true;
        Track* track = nullptr;
        uint32_t flags = 0;
        uint64_t base = 0;
        uint32_t defaultDuration = 0;
        uint32_t defaultSize = 0;
        uint32_t defaultFlags = 0;
        std::optional<uint64_t> decodeTime;
        uint64_t position = 0;
        bool sawRun = false;
        bool parsed = forEachBox(traf, [&](uint32_t type, std::span<const uint8_t> payload) {
            Reader reader(payload);
            if (type == fourCC("tfhd")) {
                flags = reader.u32() & 0xffffff;
                track = this->track(reader.u32());
                if (!track)
                    return !reader.failed();
                defaultDuration = track->defaultDuration;
                defaultSize = track->defaultSize;
                defaultFlags = track->defaultFlags;
                if (flags & 0x1)
                    base = reader.u64();
                else
                    base = (flags & 0x20000) || firstFragment ? moofOffset : previousEnd;
                if (flags & 0x2)
                    reader.skip(4);
                if (flags & 0x8)
                    defaultDuration = reader.u32();
                if (flags & 0x10)
                    defaultSize = reader.u32();
                if (flags & 0x20)
                    defaultFlags = reader.u32();
                position = base;
                return !reader.failed();
            }
            if (!track)
                return true;
            if (type == fourCC("tfdt")) {
                uint8_t version = reader.u8();
                reader.skip(3);
                decodeTime = version == 1 ? reader.u64() : reader.u32();
                return !reader.failed();
            }
            if (type != fourCC("trun"))
                return true;
            uint8_t version = reader.u8();
            uint32_t runFlags = reader.u24();
            uint32_t count = reader.u32();
            if (count > maximumRuns - m_runs.size())
                return false;
            if (runFlags & 0x1)
                position = base + static_cast<int32_t>(reader.u32());
            else if (!sawRun)
                position = base;
            std::optional<uint32_t> firstFlags;
            if (runFlags & 0x4)
                firstFlags = reader.u32();
            if (!decodeTime)
                decodeTime = track->nextDecodeTime;
            for (uint32_t index = 0; index < count && !reader.failed(); ++index) {
                uint32_t duration = runFlags & 0x100 ? reader.u32() : defaultDuration;
                uint32_t size = runFlags & 0x200 ? reader.u32() : defaultSize;
                uint32_t sampleFlags = runFlags & 0x400 ? reader.u32() : (!index && firstFlags ? *firstFlags : defaultFlags);
                int64_t compositionOffset = 0;
                if (runFlags & 0x800)
                    compositionOffset = version ? static_cast<int64_t>(static_cast<int32_t>(reader.u32())) : reader.u32();
                bool sync = track->kind == Kind::Audio || !(sampleFlags & 0x10000);
                m_runs.append({ track->id, position, size, static_cast<int64_t>(*decodeTime), compositionOffset, duration, sync });
                position += size;
                *decodeTime += duration;
            }
            sawRun = true;
            return !reader.failed();
        });
        if (!parsed)
            return false;
        if (track && decodeTime)
            track->nextDecodeTime = *decodeTime;
        previousEnd = position;
        firstFragment = false;
        return true;
    });
}

bool ISOBMFFParserSOS2::parseMediaData(std::span<const uint8_t> payload, uint64_t payloadOffset, NOESCAPE const Function<void(Sample&&)>& callback)
{
    uint64_t end = payloadOffset + payload.size();
    Vector<Run> later;
    for (auto& run : m_runs) {
        if (run.offset >= end) {
            later.append(run);
            continue;
        }
        if (run.offset < payloadOffset || run.size > end - run.offset)
            continue;
        auto* track = this->track(run.trackID);
        if (!track)
            continue;
        int64_t start = track->presentationStart;
        callback({ run.trackID, track->timescale, run.decodeTime - start, run.decodeTime + run.compositionOffset - start, run.duration, run.sync,
            Vector<uint8_t>(payload.subspan(run.offset - payloadOffset, run.size)) });
    }
    m_runs = WTF::move(later);
    return true;
}

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
