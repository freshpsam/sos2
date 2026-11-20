#pragma once

#if ENABLE(MEDIA_SOURCE)

#include "FloatSize.h"
#include "ISOBMFFParserSOS2.h"
#include "IntSize.h"
#include "NativeImage.h"
#include "PlatformImage.h"
#include <array>
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/MediaTime.h>
#include <wtf/MonotonicTime.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;
struct SwsContext;

namespace WebCore {

class MediaSampleSOS2;

// Decodes and presents what a player's source buffers enqueue. Each track
// decodes on a thread of its own with FFmpeg. Audio goes to the process's
// SOS2 audio stream, whose progress is the clock; without audio, monotonic
// time is. Video frames wait, converted to BGRA at the size the page shows
// them, until the clock reaches them.
class MediaRendererSOS2 : public ThreadSafeRefCounted<MediaRendererSOS2> {
public:
    // What the devices decode in real time: the tablet's 2 GHz Cortex-A53s
    // (seven for a page) and the phone's 1.2 GHz ones (three). Pages asking
    // about larger video (YouTube's quality menu does) are told it is
    // unsupported, and no converted frame holds more pixels.
#if CPU(ADDRESS64)
    static constexpr unsigned maximumLongSide = 1280;
    static constexpr unsigned maximumShortSide = 720;
#else
    static constexpr unsigned maximumLongSide = 854;
    static constexpr unsigned maximumShortSide = 480;
#endif

    static Ref<MediaRendererSOS2> create() { return adoptRef(*new MediaRendererSOS2); }
    ~MediaRendererSOS2();

    class Track : public ThreadSafeRefCounted<Track> {
    public:
        ~Track();

        bool isVideo() const { return m_info.kind == ISOBMFFParserSOS2::Kind::Video; }
        const ISOBMFFParserSOS2::Track& info() const { return m_info; }

        // Main thread, from the source buffer.
        void enqueue(Ref<MediaSampleSOS2>&&);
        void flush();
        void endOfStream();
        bool isReadyForMoreSamples();
        // Calls back on the main thread once the queue has room again.
        void whenReadyForMoreSamples(Function<void()>&&);

    private:
        friend class MediaRendererSOS2;
        Track(MediaRendererSOS2&, const ISOBMFFParserSOS2::Track&, bool active);

        void start();
        void stop();
        void run();
        bool openDecoder(std::span<const uint8_t> configuration);
        void decode(MediaSampleSOS2*, unsigned generation);
        void output(unsigned generation);
        void outputVideo(unsigned generation);
        void outputAudio(unsigned generation);
        bool current(unsigned generation);
        void notifyReady() WTF_REQUIRES_LOCK(m_lock);

        RefPtr<MediaRendererSOS2> m_renderer;
        const ISOBMFFParserSOS2::Track m_info;
        const bool m_active; // the renderer plays one audio and one video track
        const size_t m_queueLimit;

        Lock m_lock;
        Condition m_condition;
        Deque<Ref<MediaSampleSOS2>> m_queue WTF_GUARDED_BY_LOCK(m_lock);
        Vector<Function<void()>> m_readyCallbacks WTF_GUARDED_BY_LOCK(m_lock);
        unsigned m_generation WTF_GUARDED_BY_LOCK(m_lock) { 0 };
        bool m_endOfStream WTF_GUARDED_BY_LOCK(m_lock) { false };
        bool m_stopping WTF_GUARDED_BY_LOCK(m_lock) { false };
        // The earliest presentation time of a sample to show since the last
        // flush, in microseconds: output before it was only decoded to reach it.
        int64_t m_firstShownTime WTF_GUARDED_BY_LOCK(m_lock) { INT64_MAX };
        RefPtr<Thread> m_thread;

        // The decoder thread's own.
        AVCodecContext* m_context { nullptr };
        AVPacket* m_packet { nullptr };
        AVFrame* m_frame { nullptr };
        SwsContext* m_scaler { nullptr };
        std::array<int, 7> m_scalerSetup { }; // what m_scaler's colorspace was set for
        SwrContext* m_resampler { nullptr };
        Vector<int16_t> m_pcm;
        Vector<uint8_t> m_configuration;
        unsigned m_decoderGeneration { 0 };
        bool m_drained { false };
    };

    Ref<Track> addTrack(const ISOBMFFParserSOS2::Track&);
    // Stops a source buffer's track, which leaves room for another active one.
    void removeTrack(Track&);
    void shutdown();

    void play();
    void pause();
    // The clock waits at time until media from there arrives.
    void seek(const MediaTime&);
    MediaTime currentTime() const;
    bool timeIsProgressing() const;
    void setVolume(float volume) { m_volume = volume; }
    void setMuted(bool muted) { m_muted = muted; }
    // The size the page paints video at: frames are converted to it.
    void setOutputSize(const IntSize&);
    FloatSize naturalSize() const;
    bool hasAudio() const;
    bool hasVideo() const;

    // Main thread, at each display frame: takes the newest decoded video
    // frame the clock has reached. True when the frame to show changed.
    bool advance();
    RefPtr<NativeImage> currentFrame() const { return m_current; }
    unsigned shownFrames() const { return m_shownFrames; }
    unsigned droppedFrames() const { return m_droppedFrames; }

private:
    MediaRendererSOS2() = default;

    struct Frame {
        MediaTime time;
        PlatformImagePtr image;
        IntSize decodedSize;
    };

    bool followsAudio() const WTF_REQUIRES_LOCK(m_lock) { return m_audio && m_audioOpen && !m_audioFinished; }
    MediaTime clockTime() const WTF_REQUIRES_LOCK(m_lock);
    // Before the clock stops following audio: it goes on in monotonic time.
    void followMonotonicTime() WTF_REQUIRES_LOCK(m_lock);
    // Decoder threads: false once the track's generation moved on.
    bool pushFrame(Track&, unsigned generation, Frame&&);
    bool writeAudio(Track&, unsigned generation, int64_t time, std::span<const int16_t>);
    void audioDrained(Track&, unsigned generation);
    void audioFailed(Track&);
    void flushVideo();
    void flushAudio();
    IntSize frameSize(int width, int height) const;

    mutable Lock m_lock;
    Condition m_frameTaken;
    Condition m_resumed; // the audio decoder waits for playback while paused
    Vector<Ref<Track>> m_tracks WTF_GUARDED_BY_LOCK(m_lock);
    RefPtr<Track> m_audio WTF_GUARDED_BY_LOCK(m_lock);
    RefPtr<Track> m_video WTF_GUARDED_BY_LOCK(m_lock);
    Vector<Frame> m_frames WTF_GUARDED_BY_LOCK(m_lock);
    bool m_showFirstFrame WTF_GUARDED_BY_LOCK(m_lock) { true }; // since the last seek or flush
    bool m_playing WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_shutDown WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_audioOpen WTF_GUARDED_BY_LOCK(m_lock) { false };
    // The active audio track decoded all it had at the end of the stream.
    bool m_audioDrained WTF_GUARDED_BY_LOCK(m_lock) { false };
    // ... and all of it was heard, so the clock follows monotonic time.
    bool m_audioFinished WTF_GUARDED_BY_LOCK(m_lock) { false };
    // The clock: m_anchorTime once the audio stream reached m_anchorPosition
    // frames heard, or at m_anchorWall without audio.
    MediaTime m_anchorTime WTF_GUARDED_BY_LOCK(m_lock) { MediaTime::zeroTime() };
    bool m_audioAnchored WTF_GUARDED_BY_LOCK(m_lock) { false };
    uint64_t m_anchorPosition WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    // The most frames heard since the anchor: silence the device plays when
    // the stream runs short counts as latency until it is heard.
    mutable uint64_t m_heard WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    std::optional<MonotonicTime> m_anchorWall WTF_GUARDED_BY_LOCK(m_lock);
    IntSize m_outputSize WTF_GUARDED_BY_LOCK(m_lock);
    std::atomic<float> m_volume { 1 };
    std::atomic<bool> m_muted { false };

    // Main thread.
    RefPtr<NativeImage> m_current;
    IntSize m_decodedSize; // of the frame shown
    unsigned m_shownFrames { 0 };
    unsigned m_droppedFrames { 0 };
};

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
