#ifndef SOS2_BOARD_H
#define SOS2_BOARD_H
/* The phones' browser arena (SOS2_ARENA_*), where the resources expand and
 * the page pool lies (kernel/protection/boot_memory.c), clear of every
 * reservation the firmware hands over and of the live framebuffer too. */
#ifdef SOS2_BOARD_REDMI2
#define SOS2_RAM_START 0x80000000
#define SOS2_RAM_END   0xc0000000
/* Above MSM8916's firmware allocation window (TZ, SMEM, modem, WCNSS, venus,
 * MBA), even if the minimal handoff omits its reservations: upstream
 * qcom/msm8916.dtsi's reserved-memory and alloc-ranges. */
#define SOS2_ARENA_START 0x90000000
#define SOS2_ARENA_END   0xc0000000
#elif defined(SOS2_BOARD_GTA7LITE)
#define SOS2_RAM_START 0x40000000
#define SOS2_RAM_END   0x100000000ull
#elif defined(SOS2_BOARD_NEXUS5)
/* Two 1 GiB banks from address 0. */
#define SOS2_RAM_START   0x00000000
#define SOS2_RAM_END     0x80000000
/* The second bank: identity-mapped kernel memory must stay clear of ARM32
 * user space (0x10000000-0x40000000, include/user_abi.h), and the first
 * bank below it holds the boot data and the firmware. */
#define SOS2_ARENA_START 0x40000000
#define SOS2_ARENA_END   0x80000000
#elif defined(__x86_64__)
/* QEMU's microvm: RAM from address 0, the page pool below 4 GiB. */
#define SOS2_RAM_START 0x00000000
#define SOS2_RAM_END   0x100000000ull
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
