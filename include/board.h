#ifndef SOS2_BOARD_H
#define SOS2_BOARD_H
#ifdef SOS2_BOARD_REDMI2
#define SOS2_RAM_START 0x80000000
#define SOS2_RAM_END   0xc0000000
#elif defined(SOS2_BOARD_GTA7LITE)
#define SOS2_RAM_START 0x40000000
#define SOS2_RAM_END   0x100000000ull
#elif defined(SOS2_BOARD_NEXUS5)
/* Two 1 GiB banks from address 0. */
#define SOS2_RAM_START 0x00000000
#define SOS2_RAM_END   0x80000000
#else
#define SOS2_RAM_START 0x40000000
#ifdef __aarch64__
/* Wherever the DTB puts RAM below 4 GiB (boot_memory_available). */
#define SOS2_RAM_END 0x100000000ull
#else
#define SOS2_RAM_END 0x80000000
#endif
#endif
#endif
