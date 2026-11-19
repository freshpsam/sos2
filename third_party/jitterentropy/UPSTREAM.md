# Jitterentropy 3.7.0

Source: https://github.com/smuellerDD/jitterentropy-library
Commit: `e783cf1c450bce4d72f95c9f9c84546a6094976a`
Archive: https://codeload.github.com/smuellerDD/jitterentropy-library/tar.gz/e783cf1c450bce4d72f95c9f9c84546a6094976a
SHA-256: `bcbb2750d2f57c9bc8659a5dc4972f883ceb0a2b7504ba7a43b642ac34843278`

Distributed under the upstream BSD license choice. Source and public headers
are unmodified. The unused stdio status implementation is omitted. The
OS interface `jitterentropy-base-user.h` is SOS2's freestanding replacement,
using a private bounded allocator and ARM architectural counter. No POSIX or
Linux services are linked. All core sources compile with `-O0 -fwrapv`.

Startup and continuous health tests, including the FIPS-mode checks, remain
enabled; this is not a claim of FIPS certification. SOS2 bounds execution and
fails closed on clock/health/allocation errors. Real timing quality and
latency still require validation on the devices: the Redmi 2 (MSM8916) and
the Galaxy Tab A7 Lite (MT6765).
