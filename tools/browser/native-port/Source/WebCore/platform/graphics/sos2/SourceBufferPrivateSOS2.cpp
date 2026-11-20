#include "config.h"
#include "SourceBufferPrivateSOS2.h"

#if ENABLE(MEDIA_SOURCE)

#include "AudioTrackPrivate.h"
#include "ContentType.h"
#include "Logging.h"
#include "MediaDescription.h"
#include "MediaPlayerPrivateSOS2.h"
#include "MediaSampleSOS2.h"
#include "MediaSourcePrivateSOS2.h"
#include "SourceBufferPrivateClient.h"
#include "VideoTrackPrivate.h"
#include <wtf/NativePromise.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

namespace {

class MediaDescriptionSOS2 final : public MediaDescription {
public:
    static Ref<MediaDescriptionSOS2> create(const ISOBMFFParserSOS2::Track& track)
    {
        return adoptRef(*new MediaDescriptionSOS2(track.codec.isolatedCopy(), track.kind == ISOBMFFParserSOS2::Kind::Video));
    }

    bool isVideo() const final { return m_video; }
    bool isAudio() const final { return !m_video; }
    bool isText() const final { return false; }

private:
    MediaDescriptionSOS2(String&& codec, bool video)
        : MediaDescription(WTF::move(codec))
        , m_video(video)
    {
    }

    const bool m_video;
};

class AudioTrackPrivateSOS2 final : public AudioTrackPrivate {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(AudioTrackPrivateSOS2);
public:
    static Ref<AudioTrackPrivateSOS2> create(const ISOBMFFParserSOS2::Track& track) { return adoptRef(*new AudioTrackPrivateSOS2(track)); }
    TrackID id() const final { return m_id; }

private:
    explicit AudioTrackPrivateSOS2(const ISOBMFFParserSOS2::Track& track)
        : m_id(track.id)
    {
        setConfiguration({ { track.codec }, track.sampleRate, track.channels });
    }

    const TrackID m_id;
};

class VideoTrackPrivateSOS2 final : public VideoTrackPrivate {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(VideoTrackPrivateSOS2);
public:
    static Ref<VideoTrackPrivateSOS2> create(const ISOBMFFParserSOS2::Track& track) { return adoptRef(*new VideoTrackPrivateSOS2(track)); }
    TrackID id() const final { return m_id; }

private:
    explicit VideoTrackPrivateSOS2(const ISOBMFFParserSOS2::Track& track)
        : m_id(track.id)
    {
        PlatformVideoTrackConfiguration configuration;
        configuration.codec = track.codec;
        configuration.width = track.width;
        configuration.height = track.height;
        setConfiguration(WTF::move(configuration));
    }

    const TrackID m_id;
};

} // namespace

Ref<SourceBufferPrivateSOS2> SourceBufferPrivateSOS2::create(MediaSourcePrivateSOS2& parent, MediaRendererSOS2& renderer)
{
    return adoptRef(*new SourceBufferPrivateSOS2(parent, renderer));
}

SourceBufferPrivateSOS2::SourceBufferPrivateSOS2(MediaSourcePrivateSOS2& parent, MediaRendererSOS2& renderer)
    : SourceBufferPrivate(parent)
    , m_renderer(renderer)
#if !RELEASE_LOG_DISABLED
    , m_logger(parent.logger())
    , m_logIdentifier(parent.nextSourceBufferLogIdentifier())
#endif
{
}

SourceBufferPrivateSOS2::~SourceBufferPrivateSOS2() = default;

Ref<MediaPromise> SourceBufferPrivateSOS2::appendInternal(Ref<SharedBuffer>&& data)
{
    bool parsed = m_parser.append(data->span(), [&](ISOBMFFParserSOS2::Initialization&& initialization) {
        didReceiveInitialization(WTF::move(initialization));
    }, [&](ISOBMFFParserSOS2::Sample&& sample) {
        didReceiveSample(WTF::move(sample));
    });
    return parsed ? MediaPromise::createAndResolve() : MediaPromise::createAndReject(PlatformMediaError::ParsingError);
}

void SourceBufferPrivateSOS2::didReceiveInitialization(ISOBMFFParserSOS2::Initialization&& initialization)
{
    SourceBufferPrivateClient::InitializationSegment segment;
    segment.duration = initialization.duration > 0 ? MediaTime::createWithDouble(initialization.duration) : MediaTime::invalidTime();
    bool first = m_tracks.isEmpty();
    for (auto& track : initialization.tracks) {
        bool video = track.kind == ISOBMFFParserSOS2::Kind::Video;
        m_trackStates.set(track.id, TrackState { SharedBuffer::create(track.extradata.span()), video ? FloatSize(track.width, track.height) : FloatSize() });
        if (first)
            m_tracks.add(track.id, m_renderer->addTrack(track));
        if (video)
            segment.videoTracks.append({ MediaDescriptionSOS2::create(track), VideoTrackPrivateSOS2::create(track) });
        else
            segment.audioTracks.append({ MediaDescriptionSOS2::create(track), AudioTrackPrivateSOS2::create(track) });
    }
    SourceBufferPrivate::didReceiveInitializationSegment(WTF::move(segment));
}

void SourceBufferPrivateSOS2::didReceiveSample(ISOBMFFParserSOS2::Sample&& sample)
{
    auto state = m_trackStates.find(sample.trackID);
    if (state == m_trackStates.end())
        return;
    auto timescale = sample.timescale;
    Ref mediaSample = MediaSampleSOS2::create(SharedBuffer::create(WTF::move(sample.data)), *state->value.configuration, sample.trackID,
        MediaTime(sample.presentationTime, timescale), MediaTime(sample.decodeTime, timescale), MediaTime(sample.duration, timescale), sample.sync, state->value.size);
    SourceBufferPrivate::didReceiveSample(WTF::move(mediaSample));
}

void SourceBufferPrivateSOS2::resetParserStateInternal()
{
    m_parser.reset();
}

void SourceBufferPrivateSOS2::removedFromMediaSource()
{
    auto tracks = std::exchange(m_tracks, { });
    for (auto& track : tracks.values())
        m_renderer->removeTrack(track);
    SourceBufferPrivate::removedFromMediaSource();
}

void SourceBufferPrivateSOS2::flush(TrackID trackID)
{
    if (auto track = m_tracks.get(trackID))
        track->flush();
}

void SourceBufferPrivateSOS2::enqueueSample(Ref<MediaSample>&& sample, TrackID trackID)
{
    if (auto track = m_tracks.get(trackID))
        track->enqueue(Ref { static_cast<MediaSampleSOS2&>(sample.get()) });
}

void SourceBufferPrivateSOS2::allSamplesInTrackEnqueued(TrackID trackID)
{
    if (auto track = m_tracks.get(trackID))
        track->endOfStream();
}

bool SourceBufferPrivateSOS2::isReadyForMoreSamples(TrackID trackID)
{
    auto track = m_tracks.get(trackID);
    return track && track->isReadyForMoreSamples();
}

void SourceBufferPrivateSOS2::notifyClientWhenReadyForMoreSamples(TrackID trackID)
{
    auto track = m_tracks.get(trackID);
    if (!track)
        return;
    track->whenReadyForMoreSamples([weakThis = ThreadSafeWeakPtr { *this }, trackID] {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->provideMediaData(trackID);
    });
}

void SourceBufferPrivateSOS2::updateTrackIds(Vector<std::pair<TrackID, TrackID>>&& trackIDPairs)
{
    auto tracks = std::exchange(m_tracks, { });
    for (auto& [oldID, newID] : trackIDPairs) {
        if (auto track = tracks.take(oldID))
            m_tracks.set(newID, track.releaseNonNull());
    }
    for (auto& [id, track] : tracks)
        m_tracks.add(id, WTF::move(track));
    SourceBufferPrivate::updateTrackIds(WTF::move(trackIDPairs));
}

bool SourceBufferPrivateSOS2::canSwitchToType(const ContentType& contentType)
{
    MediaEngineSupportParameters parameters;
    parameters.isMediaSource = true;
    parameters.type = contentType;
    return MediaPlayerPrivateSOS2::supportsType(parameters) != MediaPlayer::SupportsType::IsNotSupported;
}

#if !RELEASE_LOG_DISABLED
WTFLogChannel& SourceBufferPrivateSOS2::logChannel() const
{
    return LogMediaSource;
}
#endif

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
