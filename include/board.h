#ifndef SOS2_BOARD_H
#define SOS2_BOARD_H
#ifdef SOS2_BOARD_REDMI2
#define SOS2_RAM_START  0x80000000
#define SOS2_RAM_END    0xc0000000
#define SOS2_BOARD_NAME "Redmi 2 (wt88047)"
#else
#define SOS2_RAM_START 0x40000000
#ifdef SOS2_RESOURCES
#define SOS2_RAM_END 0x80000000
#else
#define SOS2_RAM_END 0x50000000
#endif
#define SOS2_BOARD_NAME "QEMU virt"
#endif
#endif
