#pragma once

#if ENABLE(MEDIA_SOURCE)

#include "MediaPlayerPrivate.h"
#include "MediaRendererSOS2.h"
#include "PlatformTimeRanges.h"
#include <wtf/RefCounted.h>
#include <wtf/RunLoop.h>
#include <wtf/WeakPtr.h>

namespace WebCore {

class MediaSourcePrivateSOS2;

// SOS2's media engine: Media Source Extensions with fragmented MP4, H.264
// video and AAC audio (what YouTube and most streaming sites send a browser
// without VP9), decoded with FFmpeg (MediaRendererSOS2).
class MediaPlayerPrivateSOS2 final
    : public MediaPlayerPrivateInterface
    , public RefCounted<MediaPlayerPrivateSOS2>
    , public CanMakeWeakPtr<MediaPlayerPrivateSOS2> {
public:
    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    explicit MediaPlayerPrivateSOS2(MediaPlayer&);
    ~MediaPlayerPrivateSOS2();

    static void registerMediaEngine(MediaEngineRegistrar);
    static void getSupportedTypes(HashSet<String>&);
    static MediaPlayer::SupportsType supportsType(const MediaEngineSupportParameters&);

    constexpr MediaPlayerType mediaPlayerType() const final { return MediaPlayerType::SOS2; }

    MediaRendererSOS2& renderer() { return m_renderer; }
    void durationChanged();
    void activeSourceBuffersChanged();

#if !RELEASE_LOG_DISABLED
    uint64_t mediaPlayerLogIdentifier() { return m_player.get()->mediaPlayerLogIdentifier(); }
    const Logger& mediaPlayerLogger() { return m_player.get()->mediaPlayerLogger(); }
#endif

private:
    void load(const String&) final;
    void load(const URL&, const LoadOptions&, MediaSourcePrivateClient&) final;
#if ENABLE(MEDIA_STREAM)
    void load(MediaStreamPrivate&) final { }
#endif
    void cancelLoad() final;
    void play() final;
    void pause() final;
    FloatSize naturalSize() const final { return m_renderer->naturalSize(); }
    bool hasVideo() const final;
    bool hasAudio() const final;
    void setPageIsVisible(bool) final { }
    MediaTime duration() const final;
    MediaTime currentTime() const final;
    bool timeIsProgressing() const final;
    void seekToTarget(const SeekTarget&) final;
    bool seeking() const final { return !!m_pendingSeek; }
    bool paused() const final { return !m_playing; }
    void setVolume(float) final;
    void setMuted(bool) final;
    MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    MediaPlayer::ReadyState readyState() const final;
    void readyStateFromMediaSourceChanged() final;
    void characteristicsFromMediaSourceChanged() final;
    void mediaSourceHasRetrievedAllData() final;
    MediaTime maxTimeSeekable() const final { return duration(); }
    const PlatformTimeRanges& buffered() const final;
    bool didLoadingProgress() const final;
    void setPresentationSize(const IntSize&) final;
    void paint(GraphicsContext&, const FloatRect&) final;
    bool supportsAcceleratedRendering() const final;
    RefPtr<NativeImage> nativeImageForCurrentTime() final { return m_renderer->currentFrame(); }
    bool hasAvailableVideoFrame() const final { return !!m_renderer->currentFrame(); }
    DestinationColorSpace colorSpace() final;
    std::optional<VideoPlaybackQualityMetrics> videoPlaybackQualityMetrics() final;

    void tick();
    void updateRendererClock();
    void setNetworkState(MediaPlayer::NetworkState);

    ThreadSafeWeakPtr<MediaPlayer> m_player;
    const Ref<MediaRendererSOS2> m_renderer;
    RefPtr<MediaSourcePrivateSOS2> m_mediaSource;
    RunLoop::Timer m_timer;
    std::optional<SeekTarget> m_pendingSeek;
    unsigned m_seeks { 0 }; // seeks started: the latest owns m_pendingSeek
    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    FloatSize m_naturalSize; // as sizeChanged() last reported it
    mutable PlatformTimeRanges m_buffered;
    mutable MediaTime m_bufferedEnd;
    bool m_playing { false };
    bool m_ended { false };
    bool m_rendererRunning { false }; // as updateRendererClock last set it
    bool m_waitingForFrame { true }; // no frame shown since loading or seeking
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::MediaPlayerPrivateSOS2)
static bool isType(const WebCore::MediaPlayerPrivateInterface& player) { return player.mediaPlayerType() == WebCore::MediaPlayerType::SOS2; }
SPECIALIZE_TYPE_TRAITS_END()

#endif // ENABLE(MEDIA_SOURCE)
