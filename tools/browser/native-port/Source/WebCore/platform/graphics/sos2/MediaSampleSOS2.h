#pragma once

#if ENABLE(MEDIA_SOURCE)

#include "FloatSize.h"
#include "MediaSample.h"
#include "SharedBuffer.h"

namespace WebCore {

// One coded frame from ISOBMFFParserSOS2: an H.264 access unit (length-prefixed
// NAL units, as in MP4) or an AAC frame, with the decoder configuration of the
// initialization segment before it (an avcC record or AudioSpecificConfig),
// which changes when a page switches quality. Copies share the bytes.
class MediaSampleSOS2 final : public MediaSample {
public:
    static Ref<MediaSampleSOS2> create(Ref<SharedBuffer>&& data, Ref<SharedBuffer>&& configuration, TrackID trackID, const MediaTime& presentationTime, const MediaTime& decodeTime, const MediaTime& duration, bool sync, const FloatSize& size)
    {
        return adoptRef(*new MediaSampleSOS2(WTF::move(data), WTF::move(configuration), trackID, presentationTime, decodeTime, duration, sync, size));
    }

    std::span<const uint8_t> data() const { return m_data->span(); }
    const SharedBuffer& configuration() const { return m_configuration; }
    bool isSync() const { return m_sync; }
    bool isNonDisplaying() const { return m_nonDisplaying; }

    MediaTime presentationTime() const final { return m_presentationTime; }
    MediaTime decodeTime() const final { return m_decodeTime; }
    MediaTime duration() const final { return m_duration; }
    TrackID trackID() const final { return m_trackID; }
    size_t sizeInBytes() const final { return m_data->size(); }
    FloatSize presentationSize() const final { return m_size; }
    void offsetTimestampsBy(const MediaTime&) final;
    void setTimestamps(const MediaTime& presentationTime, const MediaTime& decodeTime) final;
    Ref<MediaSample> createNonDisplayingCopy() const final;
    SampleFlags flags() const final;
    PlatformSample platformSample() const final { return PlatformSample { static_cast<const MockSampleBox*>(nullptr) }; }
    Type type() const final { return Type::None; }
    void dump(PrintStream&) const final;

private:
    MediaSampleSOS2(Ref<SharedBuffer>&&, Ref<SharedBuffer>&&, TrackID, const MediaTime& presentationTime, const MediaTime& decodeTime, const MediaTime& duration, bool sync, const FloatSize&);

    const Ref<SharedBuffer> m_data;
    const Ref<SharedBuffer> m_configuration;
    const TrackID m_trackID;
    MediaTime m_presentationTime;
    MediaTime m_decodeTime;
    const MediaTime m_duration;
    const FloatSize m_size;
    const bool m_sync;
    bool m_nonDisplaying { false };
};

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
