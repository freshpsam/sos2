list(APPEND PAL_SOURCES
    crypto/sos2/CryptoDigestSOS2.cpp
    system/ClockGeneric.cpp
    system/Sound.cpp
    text/KillRing.cpp
)
list(APPEND PAL_LIBRARIES ${SOS2_MBEDCRYPTO})
