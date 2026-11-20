#include "config.h"
#include "MediaSampleSOS2.h"

#if ENABLE(MEDIA_SOURCE)

#include <wtf/PrintStream.h>

namespace WebCore {

MediaSampleSOS2::MediaSampleSOS2(Ref<SharedBuffer>&& data, Ref<SharedBuffer>&& configuration, TrackID trackID, const MediaTime& presentationTime, const MediaTime& decodeTime, const MediaTime& duration, bool sync, const FloatSize& size)
    : m_data(WTF::move(data))
    , m_configuration(WTF::move(configuration))
    , m_trackID(trackID)
    , m_presentationTime(presentationTime)
    , m_decodeTime(decodeTime)
    , m_duration(duration)
    , m_size(size)
    , m_sync(sync)
{
}

void MediaSampleSOS2::offsetTimestampsBy(const MediaTime& offset)
{
    m_presentationTime += offset;
    m_decodeTime += offset;
}

void MediaSampleSOS2::setTimestamps(const MediaTime& presentationTime, const MediaTime& decodeTime)
{
    m_presentationTime = presentationTime;
    m_decodeTime = decodeTime;
}

Ref<MediaSample> MediaSampleSOS2::createNonDisplayingCopy() const
{
    Ref copy = create(m_data.copyRef(), m_configuration.copyRef(), m_trackID, m_presentationTime, m_decodeTime, m_duration, m_sync, m_size);
    copy->m_nonDisplaying = true;
    return copy;
}

MediaSample::SampleFlags MediaSampleSOS2::flags() const
{
    unsigned flags = None;
    if (m_sync)
        flags |= IsSync;
    if (m_nonDisplaying)
        flags |= IsNonDisplaying;
    return static_cast<SampleFlags>(flags);
}

void MediaSampleSOS2::dump(PrintStream& out) const
{
    out.print("{PTS(", m_presentationTime, "), DTS(", m_decodeTime, "), duration(", m_duration, "), flags(", static_cast<int>(flags()), "), size(", m_data->size(), ")}");
}

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
