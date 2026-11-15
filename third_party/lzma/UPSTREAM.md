# LZMA decoder

`LzmaDec.c`, `LzmaDec.h`, `7zTypes.h`, `Precomp.h` and `Compiler.h`, copied
unchanged from the `C/` directory of 7-Zip 24.09, which carries Igor Pavlov's
LZMA SDK. Each file states its public-domain status in its header; the SDK
documentation says "LZMA SDK is written and placed in the public domain by
Igor Pavlov."

Source: https://github.com/ip7z/7zip/archive/refs/tags/24.09.tar.gz
Archive SHA-256: `e4757b307925227724a2044651193664ba3d04e9ac13b8e631f1667896014bbf`.

The protected Redmi 2 kernel decodes the phone image's resource ramdisk with
it (`kernel/protection/boot_bundle.c`): one raw LZMA stream with an end marker,
decoded straight into its final range, which also serves as the dictionary, so
the decoder allocates only its 16 KiB probability table. SOS2 checks the
stream's and the output's lengths and CRC-32s. A CRC detects damaged transport
data; it is not a signature. The kernel decodes in 64 KiB steps and reports
progress between them, so polled power and touch controls stay alive while the
CPU expands the browser.
