# Mbed TLS arithmetic for native Wi-Fi

Unmodified bignum/P-256 and SHA-256/HMAC-DRBG implementations from Mbed TLS
3.6.7. Only their source/header dependencies are retained. No TLS or OS layer
is linked here. Configuration, allocation, entropy seeding and zeroization
live in SOS2. The production DRBG is seeded/reseeded from healthy native
jitter entropy on the cooperative service worker.

Source: https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
SHA-256: `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`

Apache-2.0 licensed; see LICENSE and the file headers.
