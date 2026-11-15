include(PlatformJSCOnly.cmake)
list(REMOVE_ITEM WTF_LIBRARIES dl)
list(APPEND WTF_PUBLIC_HEADERS unix/UnixFileDescriptor.h)
