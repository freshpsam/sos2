#include "config.h"
#include "MediaPlayerPrivateSOS2.h"

#if ENABLE(MEDIA_SOURCE)

#include "ContentType.h"
#include "DestinationColorSpace.h"
#include "GraphicsContext.h"
#include "GraphicsContextStateSaver.h"
#include "MediaCapabilitiesDecodingInfo.h"
#include "MediaDecodingConfiguration.h"
#include "MediaDecodingType.h"
#include "MediaEngineConfigurationFactory.h"
#include "MediaSourcePrivateClient.h"
#include "MediaSourcePrivateSOS2.h"
#include "ProcessCapabilities.h"
#include "VideoPlaybackQualityMetrics.h"
#include <wtf/NativePromise.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace WebCore {

namespace {

// Pages asking about faster video than this, or larger than the renderer's
// limits, are told it is unsupported.
constexpr double maximumFramerate = 30;
constexpr Seconds frameInterval = 16_ms;

bool supportedCodec(StringView codec)
{
    if (equalLettersIgnoringASCIICase(codec, "avc1"_s) || equalLettersIgnoringASCIICase(codec, "avc3"_s))
        return true;
    if (codec.startsWithIgnoringASCIICase("avc1."_s) || codec.startsWithIgnoringASCIICase("avc3."_s)) {
        // The 8-bit 4:2:0 profiles: Baseline, Main, Extended and High.
        auto profile = parseInteger<uint8_t>(codec.substring(5, 2), 16);
        return profile && (*profile == 66 || *profile == 77 || *profile == 88 || *profile == 100);
    }
    // AAC-LC, HE-AAC and HE-AACv2.
    return equalLettersIgnoringASCIICase(codec, "mp4a.40"_s) || equalLettersIgnoringASCIICase(codec, "mp4a.40.2"_s)
        || equalLettersIgnoringASCIICase(codec, "mp4a.40.5"_s) || equalLettersIgnoringASCIICase(codec, "mp4a.40.29"_s);
}

bool withinLimits(unsigned width, unsigned height, double framerate)
{
    return std::max(width, height) <= MediaRendererSOS2::maximumLongSide && std::min(width, height) <= MediaRendererSOS2::maximumShortSide
        && framerate <= maximumFramerate;
}

// navigator.mediaCapabilities answers as MediaSource.isTypeSupported does.
void decodingConfiguration(MediaDecodingConfiguration&& configuration, MediaEngineConfigurationFactory::DecodingConfigurationCallback&& callback)
{
    auto supports = [](const String& contentType) {
        MediaEngineSupportParameters parameters;
        parameters.isMediaSource = true;
        parameters.type = ContentType(contentType);
        return MediaPlayerPrivateSOS2::supportsType(parameters) != MediaPlayer::SupportsType::IsNotSupported;
    };
    bool supported = configuration.type == MediaDecodingType::MediaSource;
    if (configuration.video) {
        auto& video = *configuration.video;
        supported = supported && supports(video.contentType) && withinLimits(video.width, video.height, video.framerate);
    }
    if (configuration.audio)
        supported = supported && supports(configuration.audio->contentType);
    MediaCapabilitiesDecodingInfo info;
    info.supported = supported;
    info.smooth = supported;
    info.configuration = WTF::move(configuration);
    callback(WTF::move(info));
}

class MediaPlayerFactorySOS2 final : public MediaPlayerFactory {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(MediaPlayerFactorySOS2);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(MediaPlayerFactorySOS2);
private:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::SOS2; }
    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final { return adoptRef(*new MediaPlayerPrivateSOS2(player)); }
    void getSupportedTypes(HashSet<String>& types) const final { MediaPlayerPrivateSOS2::getSupportedTypes(types); }
    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final { return MediaPlayerPrivateSOS2::supportsType(parameters); }
};

} // namespace

void MediaPlayerPrivateSOS2::registerMediaEngine(MediaEngineRegistrar registrar)
{
    registrar(makeUnique<MediaPlayerFactorySOS2>());
    static bool installed;
    if (!std::exchange(installed, true))
        MediaEngineConfigurationFactory::installFactory({ decodingConfiguration, nullptr });
}

void MediaPlayerPrivateSOS2::getSupportedTypes(HashSet<String>& types)
{
    types.add("video/mp4"_s);
    types.add("audio/mp4"_s);
}

MediaPlayer::SupportsType MediaPlayerPrivateSOS2::supportsType(const MediaEngineSupportParameters& parameters)
{
    if (!parameters.isMediaSource)
        return MediaPlayer::SupportsType::IsNotSupported;
    auto container = parameters.type.containerType();
    if (!equalLettersIgnoringASCIICase(container, "video/mp4"_s) && !equalLettersIgnoringASCIICase(container, "audio/mp4"_s))
        return MediaPlayer::SupportsType::IsNotSupported;
    auto dimension = [&](ASCIILiteral name) {
        return parseInteger<unsigned>(parameters.type.parameter(name)).value_or(0);
    };
    if (!withinLimits(dimension("width"_s), dimension("height"_s), parameters.type.parameter("framerate"_s).toDouble()))
        return MediaPlayer::SupportsType::IsNotSupported;
    auto codecs = parameters.type.codecs();
    if (codecs.isEmpty())
        return MediaPlayer::SupportsType::MayBeSupported;
    for (auto& codec : codecs) {
        if (!supportedCodec(codec))
            return MediaPlayer::SupportsType::IsNotSupported;
    }
    return MediaPlayer::SupportsType::IsSupported;
}

MediaPlayerPrivateSOS2::MediaPlayerPrivateSOS2(MediaPlayer& player)
    : m_player(player)
    , m_renderer(MediaRendererSOS2::create())
    , m_timer(RunLoop::mainSingleton(), "MediaPlayerPrivateSOS2::Timer"_s, this, &MediaPlayerPrivateSOS2::tick)
{
}

MediaPlayerPrivateSOS2::~MediaPlayerPrivateSOS2()
{
    m_timer.stop();
    m_renderer->shutdown();
}

void MediaPlayerPrivateSOS2::load(const String&)
{
    // Media files play once this engine reads them (only MSE so far).
    setNetworkState(MediaPlayer::NetworkState::FormatError);
}

void MediaPlayerPrivateSOS2::load(const URL&, const LoadOptions&, MediaSourcePrivateClient& client)
{
    if (RefPtr mediaSource = downcast<MediaSourcePrivateSOS2>(client.mediaSourcePrivate())) {
        mediaSource->setPlayer(this);
        m_mediaSource = WTF::move(mediaSource);
        client.reOpen();
    } else
        m_mediaSource = MediaSourcePrivateSOS2::create(*this, client);
    m_timer.startRepeating(frameInterval);
}

void MediaPlayerPrivateSOS2::cancelLoad()
{
}

void MediaPlayerPrivateSOS2::play()
{
    m_playing = true;
    m_ended = false;
    updateRendererClock();
    m_timer.startRepeating(frameInterval);
}

void MediaPlayerPrivateSOS2::pause()
{
    m_playing = false;
    updateRendererClock();
}

// The renderer's clock runs while the element plays and media is there: it
// stops where the buffered media runs out (WebCore then fires waiting), at
// the end and while seeking, and goes on as media arrives, after a loop's
// seek back to the start as well.
void MediaPlayerPrivateSOS2::updateRendererClock()
{
    bool media = m_mediaSource && (m_mediaSource->isEnded() || readyState() >= MediaPlayer::ReadyState::HaveFutureData);
    bool running = m_playing && !m_ended && !m_pendingSeek && media;
    if (std::exchange(m_rendererRunning, running) == running)
        return;
    if (running)
        m_renderer->play();
    else
        m_renderer->pause();
}

bool MediaPlayerPrivateSOS2::hasVideo() const
{
    return m_mediaSource && m_mediaSource->hasVideo();
}

bool MediaPlayerPrivateSOS2::hasAudio() const
{
    return m_mediaSource && m_mediaSource->hasAudio();
}

MediaTime MediaPlayerPrivateSOS2::duration() const
{
    return m_mediaSource ? m_mediaSource->duration() : MediaTime::zeroTime();
}

MediaTime MediaPlayerPrivateSOS2::currentTime() const
{
    if (m_pendingSeek)
        return m_pendingSeek->time;
    auto time = m_renderer->currentTime();
    auto duration = this->duration();
    return duration.isValid() && time > duration ? duration : time;
}

bool MediaPlayerPrivateSOS2::timeIsProgressing() const
{
    return m_playing && !m_pendingSeek && !m_ended && m_renderer->timeIsProgressing();
}

void MediaPlayerPrivateSOS2::seekToTarget(const SeekTarget& target)
{
    if (!m_mediaSource)
        return;
    m_pendingSeek = target;
    m_ended = false;
    updateRendererClock();
    unsigned seek = ++m_seeks;
    m_mediaSource->waitForTarget(target)->whenSettled(RunLoop::currentSingleton(), [weakThis = WeakPtr { *this }, seek](auto&& result) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis || protectedThis->m_seeks != seek)
            return;
        // A source that closes or goes away ends the seek where the clock is.
        if (!result) {
            protectedThis->m_pendingSeek.reset();
            protectedThis->updateRendererClock();
            return;
        }
        auto time = *result;
        protectedThis->m_renderer->seek(time);
        if (protectedThis->m_mediaSource)
            protectedThis->m_mediaSource->seekToTime(time);
        protectedThis->m_pendingSeek.reset();
        protectedThis->updateRendererClock();
        protectedThis->m_waitingForFrame = true;
        protectedThis->m_timer.startRepeating(frameInterval);
        if (RefPtr player = protectedThis->m_player.get()) {
            player->seeked(time);
            player->timeChanged();
        }
    });
}

void MediaPlayerPrivateSOS2::setVolume(float volume)
{
    m_renderer->setVolume(volume);
}

void MediaPlayerPrivateSOS2::setMuted(bool muted)
{
    m_renderer->setMuted(muted);
}

MediaPlayer::ReadyState MediaPlayerPrivateSOS2::readyState() const
{
    return m_mediaSource ? m_mediaSource->mediaPlayerReadyState() : MediaPlayer::ReadyState::HaveNothing;
}

void MediaPlayerPrivateSOS2::readyStateFromMediaSourceChanged()
{
    updateRendererClock();
    if (RefPtr player = m_player.get())
        player->readyStateChanged();
}

void MediaPlayerPrivateSOS2::characteristicsFromMediaSourceChanged()
{
    if (RefPtr player = m_player.get())
        player->characteristicChanged();
}

void MediaPlayerPrivateSOS2::mediaSourceHasRetrievedAllData()
{
    updateRendererClock();
    setNetworkState(MediaPlayer::NetworkState::Loaded);
}

void MediaPlayerPrivateSOS2::durationChanged()
{
    if (RefPtr player = m_player.get())
        player->durationChanged();
}

void MediaPlayerPrivateSOS2::activeSourceBuffersChanged()
{
    if (RefPtr player = m_player.get())
        player->activeSourceBuffersChanged();
}

const PlatformTimeRanges& MediaPlayerPrivateSOS2::buffered() const
{
    m_buffered = m_mediaSource ? m_mediaSource->buffered() : PlatformTimeRanges();
    return m_buffered;
}

bool MediaPlayerPrivateSOS2::didLoadingProgress() const
{
    auto end = m_mediaSource ? m_mediaSource->buffered().maximumBufferedTime() : MediaTime::zeroTime();
    return std::exchange(m_bufferedEnd, end) != end;
}

void MediaPlayerPrivateSOS2::setPresentationSize(const IntSize& size)
{
    m_renderer->setOutputSize(size);
}

void MediaPlayerPrivateSOS2::paint(GraphicsContext& context, const FloatRect& rect)
{
    if (context.paintingDisabled())
        return;
    RefPtr frame = m_renderer->currentFrame();
    if (!frame)
        return;
    FloatRect source { { }, frame->size() };
    if (source.size() != rect.size()) {
        context.drawNativeImage(*frame, rect, source);
        return;
    }
    // A frame converted to the size painted (MediaRendererSOS2::frameSize)
    // is copied at whole pixels, never filtered.
    InterpolationQualityMaintainer nearest(context, InterpolationQuality::DoNotInterpolate);
    context.drawNativeImage(*frame, FloatRect { roundedIntPoint(rect.location()), rect.size() }, source);
}

// SOS2's CPU compositor gives video a layer and draws the current frame into
// it (programs/browser/web_compositor.h). WebKit's GPU compositor, which
// accelerated buffers mean, takes video painted.
bool MediaPlayerPrivateSOS2::supportsAcceleratedRendering() const
{
    return !ProcessCapabilities::canUseAcceleratedBuffers();
}

DestinationColorSpace MediaPlayerPrivateSOS2::colorSpace()
{
    return DestinationColorSpace::SRGB();
}

std::optional<VideoPlaybackQualityMetrics> MediaPlayerPrivateSOS2::videoPlaybackQualityMetrics()
{
    unsigned dropped = m_renderer->droppedFrames();
    return VideoPlaybackQualityMetrics { m_renderer->shownFrames() + dropped, dropped, 0, 0, 0 };
}

void MediaPlayerPrivateSOS2::setNetworkState(MediaPlayer::NetworkState state)
{
    if (std::exchange(m_networkState, state) == state)
        return;
    if (RefPtr player = m_player.get())
        player->networkStateChanged();
}

// At each display frame while playing, or until the first frame after
// loading or seeking shows.
void MediaPlayerPrivateSOS2::tick()
{
    RefPtr player = m_player.get();
    if (!player)
        return;
    // The browser's video presenter may have shown frames as well.
    m_renderer->advance();
    if (unsigned shown = m_renderer->shownFrames(); shown != m_shownFrames) {
        m_shownFrames = shown;
        m_waitingForFrame = false;
        player->repaint();
    }
    if (auto size = m_renderer->naturalSize(); size != m_naturalSize) {
        m_naturalSize = size;
        player->sizeChanged();
    }
    // Not while seeking: the renderer's clock is still where the seek left.
    if (m_playing && !m_ended && !m_pendingSeek && m_mediaSource && m_mediaSource->isEnded()) {
        auto duration = this->duration();
        if (duration.isValid() && m_renderer->currentTime() >= duration) {
            m_ended = true;
            updateRendererClock();
            player->timeChanged();
        }
    }
    if ((!m_playing || m_ended) && !m_waitingForFrame)
        m_timer.stop();
}

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
