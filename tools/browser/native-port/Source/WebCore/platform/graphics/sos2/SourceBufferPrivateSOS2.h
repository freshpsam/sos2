#pragma once

#if ENABLE(MEDIA_SOURCE)

#include "FloatSize.h"
#include "ISOBMFFParserSOS2.h"
#include "MediaRendererSOS2.h"
#include "SharedBuffer.h"
#include "SourceBufferPrivate.h"
#include <wtf/HashMap.h>

namespace WebCore {

class MediaSourcePrivateSOS2;

// A source buffer of fragmented MP4: ISOBMFFParserSOS2 turns appends into
// tracks and samples for WebCore's coded frame processing, which hands the
// samples to play back to the renderer's decoders.
class SourceBufferPrivateSOS2 final : public SourceBufferPrivate {
public:
    static Ref<SourceBufferPrivateSOS2> create(MediaSourcePrivateSOS2&, MediaRendererSOS2&);
    ~SourceBufferPrivateSOS2();

    constexpr MediaPlatformType platformType() const final { return MediaPlatformType::SOS2; }

private:
    SourceBufferPrivateSOS2(MediaSourcePrivateSOS2&, MediaRendererSOS2&);

    struct TrackState {
        RefPtr<SharedBuffer> configuration;
        FloatSize size;
    };

    Ref<MediaPromise> appendInternal(Ref<SharedBuffer>&&) final;
    void resetParserStateInternal() final;
    void removedFromMediaSource() final;
    void flush(TrackID) final;
    void enqueueSample(Ref<MediaSample>&&, TrackID) final;
    void allSamplesInTrackEnqueued(TrackID) final;
    bool isReadyForMoreSamples(TrackID) final;
    void notifyClientWhenReadyForMoreSamples(TrackID) final;
    void updateTrackIds(Vector<std::pair<TrackID, TrackID>>&&) final;
    bool canSwitchToType(const ContentType&) final;
    // What a source buffer keeps, rather than WebCore's 304 MiB: about two
    // minutes of the largest video each architecture plays.
#if CPU(ADDRESS64)
    size_t platformMaximumBufferSize() const final { return 64 << 20; }
#else
    size_t platformMaximumBufferSize() const final { return 24 << 20; }
#endif

    void didReceiveInitialization(ISOBMFFParserSOS2::Initialization&&);
    void didReceiveSample(ISOBMFFParserSOS2::Sample&&);

#if !RELEASE_LOG_DISABLED
    const Logger& sourceBufferLogger() const final { return m_logger.get(); }
    uint64_t sourceBufferLogIdentifier() final { return m_logIdentifier; }
    const Logger& logger() const final { return m_logger.get(); }
    ASCIILiteral logClassName() const final { return "SourceBufferPrivateSOS2"_s; }
    uint64_t logIdentifier() const final { return m_logIdentifier; }
    WTFLogChannel& logChannel() const final;
#endif

    const Ref<MediaRendererSOS2> m_renderer;
    ISOBMFFParserSOS2 m_parser;
    // By the track IDs of the latest initialization segment, which samples
    // carry once parsed.
    HashMap<TrackID, TrackState> m_trackStates;
    // The renderer's tracks, made for the first initialization segment and
    // kept under the IDs WebCore maps later ones to.
    HashMap<TrackID, Ref<MediaRendererSOS2::Track>> m_tracks;
#if !RELEASE_LOG_DISABLED
    const Ref<const Logger> m_logger;
    const uint64_t m_logIdentifier;
#endif
};

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
