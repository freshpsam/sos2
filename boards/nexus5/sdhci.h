#ifndef SOS2_NEXUS5_SDHCI_H
#define SOS2_NEXUS5_SDHCI_H
#include <stdbool.h>
#include <stdint.h>

/* MSM8974's SD controllers (sdhci.c): an SDHCI 3.0 behind Qualcomm's
 * wrapper, started cold and run polled in PIO, for SDCC1's eMMC (emmc.c)
 * and SDCC2's Wi-Fi chip (sdio.c). Each controller's addresses: the
 * wrapper and its SDHCI registers, GCC's card and AHB clock branches and
 * the card clock's source, and its pads' control with the value set. A
 * waiting command yields to the desktop through yield when one is given
 * (only on the service worker). */
struct sdhci {
    uintptr_t core, hc, apps_cbcr, ahb_cbcr, clock_source, pads;
    uint32_t pads_mask, pads_value;
    /* How long the card may hold its busy after a write or an R1b command
     * (0: as long as a read's data, 500 ms). */
    uint32_t busy_us;
    void (*yield)(void);
    bool failed, slow;
};

/* Response types for the command register: none, R2 (136 bits), R3/R4
 * (no CRC or index), R1/R5/R6/R7, and R1b (busy). */
enum {
    SDHCI_NONE = 0,
    SDHCI_R2 = 0x09,
    SDHCI_R3 = 0x02,
    SDHCI_R1 = 0x1a,
    SDHCI_R1B = 0x1b,
};

/* The controller from cold, its card clock at 400 kHz. */
bool sdhci_start(struct sdhci *);
/* The card clock at 400 kHz or 50 MHz. */
bool sdhci_clock(struct sdhci *, bool fast);
/* Bus width (1, 4 or 8 bits) and high-speed timing in the host. */
void sdhci_bus(struct sdhci *, unsigned bits, bool high_speed);
/* A command without data: its first response word, failed when any of the
 * errors bits shows in it. */
bool sdhci_command(struct sdhci *, unsigned index, uint32_t argument, unsigned type,
                   uint32_t errors, uint32_t *response);
/* A command moving blocks of size bytes (at most 512, as many as count)
 * through the buffer port. */
bool sdhci_data(struct sdhci *, unsigned index, uint32_t argument, uint32_t errors, uint8_t *data,
                unsigned size, unsigned count, bool write);
#endif
