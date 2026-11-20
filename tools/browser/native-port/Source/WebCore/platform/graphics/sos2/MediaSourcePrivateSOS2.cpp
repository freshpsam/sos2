#include "config.h"
#include "MediaSourcePrivateSOS2.h"

#if ENABLE(MEDIA_SOURCE)

#include "ContentType.h"
#include "Logging.h"
#include "MediaPlayerPrivateSOS2.h"
#include "MediaSourcePrivateClient.h"
#include "SourceBufferPrivateSOS2.h"

namespace WebCore {

Ref<MediaSourcePrivateSOS2> MediaSourcePrivateSOS2::create(MediaPlayerPrivateSOS2& player, MediaSourcePrivateClient& client)
{
    Ref source = adoptRef(*new MediaSourcePrivateSOS2(player, client));
    client.setPrivateAndOpen(source.copyRef());
    return source;
}

MediaSourcePrivateSOS2::MediaSourcePrivateSOS2(MediaPlayerPrivateSOS2& player, MediaSourcePrivateClient& client)
    : MediaSourcePrivate(client)
    , m_player(player)
#if !RELEASE_LOG_DISABLED
    , m_logger(player.mediaPlayerLogger())
    , m_logIdentifier(player.mediaPlayerLogIdentifier())
#endif
{
#if !RELEASE_LOG_DISABLED
    client.setLogIdentifier(m_logIdentifier);
#endif
}

MediaSourcePrivateSOS2::~MediaSourcePrivateSOS2() = default;

MediaSourcePrivate::AddStatus MediaSourcePrivateSOS2::addSourceBuffer(const ContentType& contentType, const MediaSourceConfiguration&, RefPtr<SourceBufferPrivate>& outPrivate)
{
    RefPtr player = m_player.get();
    if (!player)
        return AddStatus::InvalidState;
    MediaEngineSupportParameters parameters;
    parameters.isMediaSource = true;
    parameters.type = contentType;
    if (MediaPlayerPrivateSOS2::supportsType(parameters) == MediaPlayer::SupportsType::IsNotSupported)
        return AddStatus::NotSupported;
    {
        Locker locker { m_lock };
        m_sourceBuffers.append(SourceBufferPrivateSOS2::create(*this, player->renderer()));
        outPrivate = m_sourceBuffers.last();
    }
    outPrivate->setMediaSourceDuration(duration());
    return AddStatus::Ok;
}

RefPtr<MediaPlayerPrivateInterface> MediaSourcePrivateSOS2::player() const
{
    return m_player.get();
}

void MediaSourcePrivateSOS2::setPlayer(MediaPlayerPrivateInterface* player)
{
    m_player = downcast<MediaPlayerPrivateSOS2>(player);
}

void MediaSourcePrivateSOS2::durationChanged(const MediaTime& duration)
{
    MediaSourcePrivate::durationChanged(duration);
    if (RefPtr player = m_player.get())
        player->durationChanged();
}

void MediaSourcePrivateSOS2::notifyActiveSourceBuffersChanged()
{
    if (RefPtr player = m_player.get())
        player->activeSourceBuffersChanged();
}

#if !RELEASE_LOG_DISABLED
WTFLogChannel& MediaSourcePrivateSOS2::logChannel() const
{
    return LogMediaSource;
}
#endif

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
