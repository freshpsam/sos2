# Native static WebCore embedding; no GLib, Linux ABI, window server or loader.
include(OptionsJSCOnly)
set(CMAKE_DL_LIBS "")
remove_definitions(-DBUILDING_JSCONLY__)
add_definitions(-DBUILDING_SOS2__)
set(ENABLE_WEBCORE ON)
set(ENABLE_WEBKIT OFF)
set(ENABLE_WEBKIT_LEGACY OFF)
set(ENABLE_WEBINSPECTORUI OFF)
set(ENABLE_TOOLS OFF)
set(WebCore_LIBRARY_TYPE STATIC)
set(PAL_LIBRARY_TYPE STATIC)
set(JavaScriptCore_LIBRARY_TYPE STATIC)
set(WTF_LIBRARY_TYPE STATIC)
set(bmalloc_LIBRARY_TYPE STATIC)

find_package(ICU 70.1 REQUIRED COMPONENTS data i18n uc)
# Native ICU uses static data and U_ENABLE_DYLOAD=0. FindICU otherwise adds dl
# merely because the build host is Unix; make the real static dependencies explicit.
set_property(TARGET ICU::uc PROPERTY INTERFACE_LINK_LIBRARIES ICU::data)
set_property(TARGET ICU::i18n PROPERTY INTERFACE_LINK_LIBRARIES ICU::uc)
find_package(HarfBuzz 2.7.4 REQUIRED COMPONENTS ICU)
find_package(Freetype REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(WebP REQUIRED COMPONENTS demux mux)
find_library(SOS2_SHARPYUV sharpyuv REQUIRED)
set_property(TARGET WebP::libwebp APPEND PROPERTY INTERFACE_LINK_LIBRARIES "${SOS2_SHARPYUV}")
find_package(LibXml2 REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(ZLIB REQUIRED)
if (NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif ()
find_library(SOS2_MBEDCRYPTO mbedcrypto REQUIRED)
find_library(SOS2_MBEDTLS mbedtls REQUIRED)
find_library(SOS2_MBEDX509 mbedx509 REQUIRED)

# WebCore also gates native mouse selection/autoscroll behind DRAG_SUPPORT.
# The embedding's empty DragClient still declines OS data-transfer sessions.
# Coordinated graphics brings WebKit's Skia compositor, which pages with a GPU
# use (programs/browser/web_gpu_compositor.h); EGL and OpenGL ES come from static Mesa.
foreach (feature USE_SKIA USE_HARFBUZZ USE_FONT_VARIANT_VIA_FEATURES USE_THEME_ADWAITA ENABLE_DRAG_SUPPORT
                 USE_TEXTURE_MAPPER USE_COORDINATED_GRAPHICS)
    SET_AND_EXPOSE_TO_BUILD(${feature} TRUE)
endforeach ()
foreach (feature USE_GLIB USE_SOUP USE_CURL USE_CAIRO USE_FREETYPE USE_LIBEPOXY USE_EGL USE_OPENGL USE_OPENGL_ES
                 USE_GSTREAMER USE_ATSPI USE_ATK
                 ENABLE_WEB_CRYPTO ENABLE_ACCESSIBILITY USE_UNIX_DOMAIN_SOCKETS)
    SET_AND_EXPOSE_TO_BUILD(${feature} FALSE)
endforeach ()
