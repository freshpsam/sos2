# hostap authentication core

wpa_supplicant 2.12, BSD licensed. Only the RSN supplicant, SAE, protocol
helpers, cryptographic primitives, and their header dependencies are retained.
No POSIX driver, Linux interface, sockets, process launcher or daemon is built.

Source: https://w1.fi/releases/wpa_supplicant-2.12.tar.gz
SHA-256: `08e23937e16d0155e55cab2b51f51fbe10d80a1aa91c4e15442645059b737ef6`

SOS2 supplies bounded allocation, timers, entropy and Mbed TLS P-256 arithmetic.
Local adaptations are the includes.h platform branch and a CONFIG_SOS2 hook
in PBKDF2 that yields to the desktop every 64 rounds. Derivation stays on the
serialized service worker; its state and password never enter a web process.
