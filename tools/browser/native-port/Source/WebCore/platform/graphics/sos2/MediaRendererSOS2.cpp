#include "config.h"
#include "MediaRendererSOS2.h"

#if ENABLE(MEDIA_SOURCE)

#include "MediaSampleSOS2.h"
#include <sos2/services.h>
#include <unistd.h>
#include <wtf/MainThread.h>
#include <wtf/Seconds.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkPixmap.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace WebCore {

namespace {

// Decoded frames waiting for the clock, and coded samples waiting for a
// decoder: about half a second of video, a second and a half of AAC.
constexpr size_t maximumFrames = 4;
constexpr size_t videoQueueLimit = 16;
constexpr size_t audioQueueLimit = 64;

int64_t microseconds(const MediaTime& time)
{
    return time.toTimeScale(1000000).timeValue();
}

// The process has one audio stream: the first renderer with audio owns it
// until it shuts down, and others follow monotonic time, silent.
Lock audioOwnerLock;
const void* audioOwner WTF_GUARDED_BY_LOCK(audioOwnerLock);

bool acquireAudio(const void* renderer)
{
    Locker locker { audioOwnerLock };
    if (audioOwner || sos2_audio(USER_AUDIO_OPEN, 0, 0) < 0)
        return false;
    audioOwner = renderer;
    return true;
}

void releaseAudio(const void* renderer)
{
    Locker locker { audioOwnerLock };
    if (audioOwner != renderer)
        return;
    sos2_audio(USER_AUDIO_CLOSE, 0, 0);
    audioOwner = nullptr;
}

user_audio_status audioStatus()
{
    user_audio_status status { };
    sos2_audio(USER_AUDIO_STATUS, reinterpret_cast<uintptr_t>(&status), 0);
    return status;
}

// Frames of this process's stream heard so far.
uint64_t heardFrames()
{
    auto status = audioStatus();
    return status.played > status.latency ? status.played - status.latency : 0;
}

} // namespace

MediaRendererSOS2::~MediaRendererSOS2()
{
    shutdown();
}

Ref<MediaRendererSOS2::Track> MediaRendererSOS2::addTrack(const ISOBMFFParserSOS2::Track& info)
{
    Locker locker { m_lock };
    bool video = info.kind == ISOBMFFParserSOS2::Kind::Video;
    bool active = !m_shutDown && (video ? !m_video : !m_audio);
    Ref track = adoptRef(*new Track(*this, info, active));
    if (m_shutDown) {
        Locker trackLocker { track->m_lock };
        track->m_stopping = true;
        return track;
    }
    if (active && video)
        m_video = track.ptr();
    else if (active) {
        // The clock waits for this track's audio from where it is.
        m_anchorTime = clockTime();
        m_anchorWall.reset();
        m_audio = track.ptr();
        m_audioDrained = false;
        m_audioFinished = false;
        if (!m_audioOpen) {
            m_audioOpen = acquireAudio(this);
            if (m_audioOpen && !m_playing)
                sos2_audio(USER_AUDIO_PAUSE, 1, 0);
        }
    }
    m_tracks.append(track);
    track->start();
    return track;
}

void MediaRendererSOS2::removeTrack(Track& track)
{
    {
        Locker locker { m_lock };
        if (m_video == &track) {
            m_video = nullptr;
            m_frames.clear();
            m_showFirstFrame = true;
            m_frameTaken.notifyAll();
        } else if (m_audio == &track) {
            followMonotonicTime();
            m_audio = nullptr;
            if (m_audioOpen)
                sos2_audio(USER_AUDIO_FLUSH, 0, 0);
            m_audioAnchored = false;
            m_resumed.notifyAll();
        }
        if (!m_tracks.removeFirstMatching([&](auto& item) { return item.ptr() == &track; }))
            return;
    }
    track.stop();
}

void MediaRendererSOS2::shutdown()
{
    Vector<Ref<Track>> tracks;
    {
        Locker locker { m_lock };
        m_shutDown = true;
        tracks = WTF::move(m_tracks);
        m_audio = nullptr;
        m_video = nullptr;
        m_frames.clear();
        m_frameTaken.notifyAll();
        m_resumed.notifyAll();
    }
    for (auto& track : tracks)
        track->stop();
    Locker locker { m_lock };
    if (std::exchange(m_audioOpen, false))
        releaseAudio(this);
}

void MediaRendererSOS2::play()
{
    Locker locker { m_lock };
    m_playing = true;
    if (m_audioOpen)
        sos2_audio(USER_AUDIO_PAUSE, 0, 0);
    m_resumed.notifyAll();
}

void MediaRendererSOS2::pause()
{
    Locker locker { m_lock };
    if (m_playing && !followsAudio()) {
        m_anchorTime = clockTime();
        m_anchorWall.reset();
    }
    m_playing = false;
    if (m_audioOpen)
        sos2_audio(USER_AUDIO_PAUSE, 1, 0);
}

void MediaRendererSOS2::seek(const MediaTime& time)
{
    Locker locker { m_lock };
    m_anchorTime = time;
    m_audioAnchored = false;
    m_audioDrained = false;
    m_audioFinished = false;
    m_anchorWall.reset();
    m_frames.clear();
    m_showFirstFrame = true;
    m_frameTaken.notifyAll();
    m_resumed.notifyAll();
}

MediaTime MediaRendererSOS2::clockTime() const
{
    if (followsAudio()) {
        if (!m_audioAnchored)
            return m_anchorTime;
        m_heard = std::max(m_heard, heardFrames());
        return m_anchorTime + MediaTime(m_heard > m_anchorPosition ? m_heard - m_anchorPosition : 0, USER_AUDIO_RATE);
    }
    if (!m_anchorWall)
        return m_anchorTime;
    return m_anchorTime + MediaTime::createWithDouble((MonotonicTime::now() - *m_anchorWall).seconds());
}

void MediaRendererSOS2::followMonotonicTime()
{
    m_anchorTime = clockTime();
    m_anchorWall = m_playing ? std::optional { MonotonicTime::now() } : std::nullopt;
}

MediaTime MediaRendererSOS2::currentTime() const
{
    Locker locker { m_lock };
    return clockTime();
}

bool MediaRendererSOS2::timeIsProgressing() const
{
    Locker locker { m_lock };
    return m_playing && (followsAudio() ? m_audioAnchored : !!m_anchorWall);
}

void MediaRendererSOS2::setOutputSize(const IntSize& size)
{
    Locker locker { m_lock };
    m_outputSize = size;
}

FloatSize MediaRendererSOS2::naturalSize() const
{
    if (!m_decodedSize.isEmpty())
        return m_decodedSize;
    Locker locker { m_lock };
    return m_video ? FloatSize(m_video->info().width, m_video->info().height) : FloatSize();
}

bool MediaRendererSOS2::hasAudio() const
{
    Locker locker { m_lock };
    return !!m_audio;
}

bool MediaRendererSOS2::hasVideo() const
{
    Locker locker { m_lock };
    return !!m_video;
}

// The size the page paints video at, so that conversion on this decoder
// thread does all the scaling and the page's paint copies frames as they are.
// Sizes larger than the largest video decoded keep the frame's own: Skia then
// scales what the page shows of it.
IntSize MediaRendererSOS2::frameSize(int width, int height) const
{
    Locker locker { m_lock };
    IntSize box = m_outputSize;
    if (box.isEmpty() || box.unclampedArea() > uint64_t { maximumLongSide } * maximumShortSide)
        return { width, height };
    return box;
}

bool MediaRendererSOS2::advance()
{
    std::optional<Frame> shown;
    {
        Locker locker { m_lock };
        // Audio that ended before the video leaves the rest to monotonic time.
        if (followsAudio() && m_audioDrained) {
            auto status = audioStatus();
            if (!status.queued && !status.latency) {
                followMonotonicTime();
                m_audioFinished = true;
            }
        }
        // Without audio, the clock starts with the first frame to show.
        if (m_playing && !m_anchorWall && !followsAudio() && (!m_frames.isEmpty() || m_current))
            m_anchorWall = MonotonicTime::now();
        MediaTime now = clockTime();
        if (m_showFirstFrame && !m_frames.isEmpty()) {
            shown = WTF::move(m_frames[0]);
            m_frames.removeAt(0);
            m_showFirstFrame = false;
        }
        while (m_frames.size() > 1 && m_frames[1].time <= now) {
            m_frames.removeAt(0);
            ++m_droppedFrames;
        }
        if (!m_frames.isEmpty() && m_frames[0].time <= now) {
            if (shown)
                ++m_droppedFrames;
            shown = WTF::move(m_frames[0]);
            m_frames.removeAt(0);
        }
        if (shown)
            m_frameTaken.notifyAll();
    }
    if (!shown)
        return false;
    m_current = NativeImage::create(WTF::move(shown->image));
    m_decodedSize = shown->decodedSize;
    ++m_shownFrames;
    return true;
}

bool MediaRendererSOS2::pushFrame(Track& track, unsigned generation, Frame&& frame)
{
    Locker locker { m_lock };
    while (m_frames.size() >= maximumFrames && m_video == &track && track.current(generation))
        m_frameTaken.wait(m_lock);
    if (m_video != &track || !track.current(generation))
        return false;
    m_frames.append(WTF::move(frame));
    return true;
}

bool MediaRendererSOS2::writeAudio(Track& track, unsigned generation, int64_t time, std::span<const int16_t> pcm)
{
    while (!pcm.empty()) {
        size_t written;
        {
            // Under the lock, so that a flush cannot slip between the
            // generation check and the write.
            Locker locker { m_lock };
            if (!m_audioOpen || m_audio != &track || !track.current(generation))
                return false;
            if (!m_audioAnchored) {
                // These frames are heard once those already queued are, and
                // the clock never goes back.
                auto status = audioStatus();
                m_anchorTime = std::max(m_anchorTime, MediaTime(time, 1000000));
                m_anchorPosition = status.played + status.queued;
                m_heard = 0;
                m_audioAnchored = true;
            }
            m_audioDrained = false;
            int result = sos2_audio(USER_AUDIO_WRITE, reinterpret_cast<uintptr_t>(pcm.data()), pcm.size() / USER_AUDIO_CHANNELS);
            if (result < 0)
                return false;
            written = static_cast<size_t>(result) * USER_AUDIO_CHANNELS;
        }
        pcm = pcm.subspan(written);
        if (pcm.empty())
            break;
        {
            // A paused stream does not drain: wait for playback.
            Locker locker { m_lock };
            while (!m_playing && m_audioOpen && m_audio == &track && track.current(generation))
                m_resumed.wait(m_lock);
        }
        // A full stream drains in real time: as long as the rest plays.
        sleep(Seconds(static_cast<double>(pcm.size() / USER_AUDIO_CHANNELS) / USER_AUDIO_RATE));
    }
    return true;
}

void MediaRendererSOS2::flushVideo()
{
    Locker locker { m_lock };
    m_frames.clear();
    m_showFirstFrame = true;
    m_frameTaken.notifyAll();
}

void MediaRendererSOS2::flushAudio()
{
    Locker locker { m_lock };
    // The clock waits where it is for the audio enqueued next.
    m_anchorTime = clockTime();
    if (m_audioOpen)
        sos2_audio(USER_AUDIO_FLUSH, 0, 0);
    m_audioAnchored = false;
    m_audioDrained = false;
    if (std::exchange(m_audioFinished, false))
        m_anchorWall.reset();
    m_resumed.notifyAll();
}

void MediaRendererSOS2::audioDrained(Track& track, unsigned generation)
{
    Locker locker { m_lock };
    if (m_audio == &track && track.current(generation))
        m_audioDrained = true;
}

// Without a decoder, the stream would never start the clock.
void MediaRendererSOS2::audioFailed(Track& track)
{
    Locker locker { m_lock };
    if (m_audio != &track || !m_audioOpen)
        return;
    followMonotonicTime();
    m_audioOpen = false;
    releaseAudio(this);
}

MediaRendererSOS2::Track::Track(MediaRendererSOS2& renderer, const ISOBMFFParserSOS2::Track& info, bool active)
    : m_renderer(&renderer)
    , m_info(info)
    , m_active(active)
    , m_queueLimit(info.kind == ISOBMFFParserSOS2::Kind::Video ? videoQueueLimit : audioQueueLimit)
{
}

MediaRendererSOS2::Track::~Track() = default;

void MediaRendererSOS2::Track::start()
{
    m_thread = Thread::create(isVideo() ? "SOS2 video decoder"_s : "SOS2 audio decoder"_s, [protectedThis = Ref { *this }] {
        protectedThis->run();
    });
}

void MediaRendererSOS2::Track::stop()
{
    {
        Locker locker { m_lock };
        m_stopping = true;
        m_queue.clear();
        m_readyCallbacks.clear();
        m_condition.notifyAll();
    }
    if (RefPtr thread = std::exchange(m_thread, nullptr))
        thread->waitForCompletion();
    m_renderer = nullptr;
}

void MediaRendererSOS2::Track::enqueue(Ref<MediaSampleSOS2>&& sample)
{
    Locker locker { m_lock };
    if (m_stopping)
        return;
    if (!sample->isNonDisplaying())
        m_firstShownTime = std::min(m_firstShownTime, microseconds(sample->presentationTime()));
    m_queue.append(WTF::move(sample));
    m_endOfStream = false;
    m_condition.notifyAll();
}

void MediaRendererSOS2::Track::flush()
{
    {
        Locker locker { m_lock };
        ++m_generation;
        m_queue.clear();
        m_endOfStream = false;
        m_firstShownTime = INT64_MAX;
        m_condition.notifyAll();
        notifyReady();
    }
    RefPtr renderer = m_renderer;
    if (!m_active || !renderer)
        return;
    if (isVideo())
        renderer->flushVideo();
    else
        renderer->flushAudio();
}

void MediaRendererSOS2::Track::endOfStream()
{
    Locker locker { m_lock };
    m_endOfStream = true;
    m_condition.notifyAll();
}

bool MediaRendererSOS2::Track::isReadyForMoreSamples()
{
    Locker locker { m_lock };
    return m_queue.size() < m_queueLimit;
}

void MediaRendererSOS2::Track::whenReadyForMoreSamples(Function<void()>&& callback)
{
    Locker locker { m_lock };
    if (m_stopping)
        return;
    m_readyCallbacks.append(WTF::move(callback));
    notifyReady();
}

// Once the queue is half empty, so that the source buffer refills it in
// batches rather than one sample per decode.
void MediaRendererSOS2::Track::notifyReady()
{
    if (m_readyCallbacks.isEmpty() || m_queue.size() > m_queueLimit / 2)
        return;
    callOnMainThread([callbacks = std::exchange(m_readyCallbacks, { })]() mutable {
        for (auto& callback : callbacks)
            callback();
    });
}

bool MediaRendererSOS2::Track::current(unsigned generation)
{
    Locker locker { m_lock };
    return !m_stopping && generation == m_generation;
}

bool MediaRendererSOS2::Track::openDecoder(std::span<const uint8_t> configuration)
{
    avcodec_free_context(&m_context);
    swr_free(&m_resampler);
    m_configuration = Vector<uint8_t>(configuration);
    const AVCodec* codec = avcodec_find_decoder(isVideo() ? AV_CODEC_ID_H264 : AV_CODEC_ID_AAC);
    m_context = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!m_packet)
        m_packet = av_packet_alloc();
    if (!m_frame)
        m_frame = av_frame_alloc();
    if (!m_context || !m_packet || !m_frame)
        return false;
    if (!configuration.empty()) {
        m_context->extradata = static_cast<uint8_t*>(av_mallocz(configuration.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!m_context->extradata)
            return false;
        memcpy(m_context->extradata, configuration.data(), configuration.size());
        m_context->extradata_size = static_cast<int>(configuration.size());
    }
    m_context->pkt_timebase = AVRational { 1, 1000000 };
    if (isVideo()) {
        // Frames decode in parallel on the CPUs the page has, four at most.
        m_context->thread_count = std::clamp(static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)), 1, 4);
        m_context->thread_type = FF_THREAD_FRAME;
    }
    return avcodec_open2(m_context, codec, nullptr) >= 0;
}

void MediaRendererSOS2::Track::run()
{
    bool opened = m_active && openDecoder(m_info.extradata.span());
    if (m_active && !opened && !isVideo())
        m_renderer->audioFailed(*this);
    while (true) {
        RefPtr<MediaSampleSOS2> sample;
        unsigned generation;
        {
            Locker locker { m_lock };
            while (!m_stopping && m_queue.isEmpty() && !(m_endOfStream && !m_drained))
                m_condition.wait(m_lock);
            if (m_stopping)
                break;
            generation = m_generation;
            if (!m_queue.isEmpty()) {
                sample = m_queue.takeFirst();
                notifyReady();
            }
        }
        // A quality switch brings its own decoder configuration.
        if (m_active && sample && !equalSpans(sample->configuration().span(), m_configuration.span())) {
            opened = openDecoder(sample->configuration().span());
            m_drained = false;
            if (!opened && !isVideo())
                m_renderer->audioFailed(*this);
        }
        // Inactive or without a decoder, a track only consumes its samples.
        if (!opened) {
            if (!sample)
                m_drained = true;
            continue;
        }
        if (generation != m_decoderGeneration || (m_drained && sample)) {
            avcodec_flush_buffers(m_context);
            if (generation != m_decoderGeneration)
                swr_free(&m_resampler);
            m_decoderGeneration = generation;
            m_drained = false;
        }
        decode(sample.get(), generation);
        if (!sample) {
            m_drained = true;
            if (!isVideo())
                m_renderer->audioDrained(*this, generation);
        }
    }
    avcodec_free_context(&m_context);
    av_packet_free(&m_packet);
    av_frame_free(&m_frame);
    sws_freeContext(std::exchange(m_scaler, nullptr));
    swr_free(&m_resampler);
}

// One sample, or with none the decoder's remaining frames at the end of
// the stream.
void MediaRendererSOS2::Track::decode(MediaSampleSOS2* sample, unsigned generation)
{
    if (sample) {
        auto data = sample->data();
        if (av_new_packet(m_packet, static_cast<int>(data.size())) < 0)
            return;
        memcpy(m_packet->data, data.data(), data.size());
        m_packet->pts = microseconds(sample->presentationTime());
        m_packet->dts = microseconds(sample->decodeTime());
        m_packet->duration = microseconds(sample->duration());
        if (sample->isSync())
            m_packet->flags |= AV_PKT_FLAG_KEY;
    }
    // A decoder that is full takes the packet once its frames are taken.
    int result;
    while ((result = avcodec_send_packet(m_context, sample ? m_packet : nullptr)) == AVERROR(EAGAIN))
        output(generation);
    av_packet_unref(m_packet);
    if (result >= 0)
        output(generation);
}

void MediaRendererSOS2::Track::output(unsigned generation)
{
    while (avcodec_receive_frame(m_context, m_frame) >= 0) {
        if (current(generation)) {
            if (isVideo())
                outputVideo(generation);
            else
                outputAudio(generation);
        }
        av_frame_unref(m_frame);
    }
}

void MediaRendererSOS2::Track::outputVideo(unsigned generation)
{
    int64_t time = m_frame->best_effort_timestamp != AV_NOPTS_VALUE ? m_frame->best_effort_timestamp : m_frame->pts;
    {
        Locker locker { m_lock };
        if (time < m_firstShownTime)
            return;
    }
    RefPtr renderer = m_renderer;
    if (!renderer)
        return;
    IntSize size = renderer->frameSize(m_frame->width, m_frame->height);
    m_scaler = sws_getCachedContext(m_scaler, m_frame->width, m_frame->height, static_cast<AVPixelFormat>(m_frame->format),
        size.width(), size.height(), AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_scaler)
        return;
    // HD video is BT.709, which the scaler does not assume.
    bool fullRange = m_frame->color_range == AVCOL_RANGE_JPEG;
    int colorspace = m_frame->colorspace == AVCOL_SPC_BT709 || (m_frame->colorspace == AVCOL_SPC_UNSPECIFIED && m_frame->height > 576) ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
    std::array<int, 7> setup { m_frame->width, m_frame->height, m_frame->format, size.width(), size.height(), colorspace, fullRange };
    if (setup != m_scalerSetup) {
        sws_setColorspaceDetails(m_scaler, sws_getCoefficients(colorspace), fullRange, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
        m_scalerSetup = setup;
    }
    size_t stride = static_cast<size_t>(size.width()) * 4;
    auto* pixels = static_cast<uint8_t*>(fastMalloc(stride * size.height()));
    uint8_t* planes[4] = { pixels, nullptr, nullptr, nullptr };
    int strides[4] = { static_cast<int>(stride), 0, 0, 0 };
    sws_scale(m_scaler, m_frame->data, m_frame->linesize, 0, m_frame->height, planes, strides);
    auto info = SkImageInfo::Make(size.width(), size.height(), kBGRA_8888_SkColorType, kOpaque_SkAlphaType, SkColorSpace::MakeSRGB());
    auto image = SkImages::RasterFromPixmap(SkPixmap(info, pixels, stride), [](const void* pixels, void*) {
        fastFree(const_cast<void*>(pixels));
    }, nullptr);
    renderer->pushFrame(*this, generation, { MediaTime(time, 1000000), WTF::move(image), { m_frame->width, m_frame->height } });
}

void MediaRendererSOS2::Track::outputAudio(unsigned generation)
{
    int64_t time = m_frame->best_effort_timestamp != AV_NOPTS_VALUE ? m_frame->best_effort_timestamp : m_frame->pts;
    {
        Locker locker { m_lock };
        if (time < m_firstShownTime)
            return;
    }
    RefPtr renderer = m_renderer;
    if (!renderer)
        return;
    if (!m_resampler) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&m_resampler, &stereo, AV_SAMPLE_FMT_S16, USER_AUDIO_RATE, &m_frame->ch_layout,
            static_cast<AVSampleFormat>(m_frame->format), m_frame->sample_rate, 0, nullptr) < 0 || swr_init(m_resampler) < 0) {
            swr_free(&m_resampler);
            return;
        }
    }
    int capacity = swr_get_out_samples(m_resampler, m_frame->nb_samples);
    if (capacity <= 0)
        return;
    m_pcm.resize(static_cast<size_t>(capacity) * USER_AUDIO_CHANNELS);
    auto* out = reinterpret_cast<uint8_t*>(m_pcm.mutableSpan().data());
    int count = swr_convert(m_resampler, &out, capacity, const_cast<const uint8_t**>(m_frame->extended_data), m_frame->nb_samples);
    if (count <= 0)
        return;
    auto pcm = m_pcm.mutableSpan().first(static_cast<size_t>(count) * USER_AUDIO_CHANNELS);
    float volume = renderer->m_muted ? 0 : renderer->m_volume.load();
    if (volume < 1) {
        for (auto& value : pcm)
            value = static_cast<int16_t>(value * volume);
    }
    renderer->writeAudio(*this, generation, time, pcm);
}

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
