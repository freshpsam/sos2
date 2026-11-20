include(platform/ImageDecoders.cmake)
include(platform/Skia.cmake)
include(platform/Adwaita.cmake)
include(platform/TextureMapper.cmake)

# Coordinated layers without a scrolling thread: WebCore scrolls them on the
# page thread, so the coordinated scrolling tree (ASYNC_SCROLLING) is left out.
list(REMOVE_ITEM WebCore_SOURCES
    page/scrolling/coordinated/ScrollerCoordinated.cpp
    page/scrolling/coordinated/ScrollerPairCoordinated.cpp
    page/scrolling/coordinated/ScrollingStateNodeCoordinated.cpp
    page/scrolling/coordinated/ScrollingStateScrollingNodeCoordinated.cpp
    page/scrolling/coordinated/ScrollingTreeCoordinated.cpp
    page/scrolling/coordinated/ScrollingTreeFixedNodeCoordinated.cpp
    page/scrolling/coordinated/ScrollingTreeFrameScrollingNodeCoordinated.cpp
    page/scrolling/coordinated/ScrollingTreeOverflowScrollProxyNodeCoordinated.cpp
    page/scrolling/coordinated/ScrollingTreeOverflowScrollingNodeCoordinated.cpp
    page/scrolling/coordinated/ScrollingTreePositionedNodeCoordinated.cpp
    page/scrolling/coordinated/ScrollingTreeScrollingNodeDelegateCoordinated.cpp
    page/scrolling/coordinated/ScrollingTreeStickyNodeCoordinated.cpp
)
list(REMOVE_ITEM WebCore_PRIVATE_FRAMEWORK_HEADERS
    page/scrolling/coordinated/ScrollingTreeCoordinated.h
)
# YUV layer buffers carry GStreamer video frames; SOS2's media engine paints
# its own (platform/graphics/sos2).
list(REMOVE_ITEM WebCore_SOURCES
    platform/graphics/texmap/coordinated/CoordinatedPlatformLayerBufferYUV.cpp
)

list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    "${WEBCORE_DIR}/platform/text/icu"
    "${WEBCORE_DIR}/platform/graphics/opentype"
    "${WEBCORE_DIR}/platform/graphics/egl"
    "${WEBCORE_DIR}/platform/graphics/opengl"
    "${WEBCORE_DIR}/platform/graphics/texmap"
    "${WEBCORE_DIR}/platform/graphics/sos2"
    "${WEBCORE_DIR}/platform/network/sos2"
    "${WEBCORE_DIR}/platform/video-codecs"
    "${THIRDPARTY_DIR}/ANGLE/include"
)
list(APPEND WebCore_SOURCES
    platform/generic/ScrollbarsControllerGeneric.cpp
    platform/graphics/opentype/OpenTypeVerticalData.cpp
    platform/text/Hyphenation.cpp
    platform/text/LocaleICU.cpp
    platform/unix/LoggingUnix.cpp
    platform/graphics/sos2/ISOBMFFParserSOS2.cpp
    platform/graphics/sos2/MediaPlayerPrivateSOS2.cpp
    platform/graphics/sos2/MediaRendererSOS2.cpp
    platform/graphics/sos2/MediaSampleSOS2.cpp
    platform/graphics/sos2/MediaSourcePrivateSOS2.cpp
    platform/graphics/sos2/SourceBufferPrivateSOS2.cpp
    platform/sos2/PlatformScreenSOS2.cpp
    platform/sos2/PlatformDisplaySOS2.cpp
    platform/graphics/PlatformDisplay.cpp
    platform/graphics/egl/GLContext.cpp
    platform/graphics/egl/GLContextWrapper.cpp
    platform/graphics/egl/GLDisplay.cpp
    platform/graphics/egl/GLFence.cpp
    platform/sos2/SystemFontDatabaseSOS2.cpp
    platform/sos2/MIMETypeRegistrySOS2.cpp
    platform/sos2/CryptoSOS2.cpp
    platform/sos2/SharedMemorySOS2.cpp
    platform/sos2/PlatformSupportSOS2.cpp
    platform/sos2/PasteboardSOS2.cpp
    platform/sos2/DragDataSOS2.cpp
    platform/skia/DragImageSkia.cpp
    editing/sos2/EditorSOS2.cpp
    platform/network/sos2/NetworkTypesSOS2.cpp
    platform/network/sos2/NetworkUnavailableSOS2.cpp
)
list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
    platform/sos2/PlatformDisplaySOS2.h
    platform/network/sos2/CertificateInfo.h
    platform/network/sos2/AuthenticationChallenge.h
    platform/network/sos2/ResourceRequest.h
    platform/network/sos2/ResourceResponse.h
    platform/network/sos2/ResourceError.h
)
# Unused GPU textures WebKit keeps for reuse (a page shows 480x752 pixels).
list(APPEND WebCore_PRIVATE_DEFINITIONS
    BITMAP_TEXTURE_POOL_MAX_SIZE_IN_MB=8
)
