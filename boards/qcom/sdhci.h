#ifndef SOS2_QCOM_SDHCI_H
#define SOS2_QCOM_SDHCI_H
#include <stdbool.h>
#include <stdint.h>

/* Qualcomm's SD controllers (sdhci.c): an SDHCI 3.0 behind Qualcomm's
 * wrapper, run polled in PIO, for the Redmi 2's eMMC
 * (boards/redmi2/emmc.c), which takes lk2nd's controller over, and the
 * Nexus 5's eMMC and Wi-Fi chip (boards/nexus5/emmc.c, sdio.c), started
 * cold. Each controller's addresses: the wrapper and its SDHCI registers,
 * GCC's card and AHB clock branches and the card clock's source, and its
 * pads' control with the value set (the last four for sdhci_start alone).
 * A waiting command yields to the desktop through yield when one is given
 * (only on the service worker). The settings a host may change, 0 for the
 * default: how long a command may take (100 ms) and a block's data (500
 * ms); how long the card may hold its busy after a write or an R1b command
 * (as long as a block's data); GPLL0's divider for 50 MHz (12, MSM8974's;
 * MSM8916's is 16); every command waiting for the data line too, not only
 * those with a busy response; and the buffer's own ready bit checked before
 * each block moves; and a multi-block transfer stopped by the host's own
 * CMD12 (an eMMC's CMD18; an SDIO card's CMD53 has no stop). */
struct sdhci {
    uintptr_t core, hc, apps_cbcr, ahb_cbcr, clock_source, pads;
    uint32_t pads_mask, pads_value;
    uint32_t command_us, data_us, busy_us;
    unsigned fast_divider;
    bool wait_data_line, check_buffer, auto_stop;
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

/* MSM8974's controller from cold, its card clock at 400 kHz. */
bool sdhci_start(struct sdhci *);
/* The card clock at 400 kHz or 50 MHz, stopped meanwhile; or GCC's rate
 * alone, which a host changing its own timing around it sets. */
bool sdhci_clock(struct sdhci *, bool fast);
bool sdhci_clock_rate(struct sdhci *, bool fast);
/* Bus width (1, 4 or 8 bits) and high-speed timing in the host. */
void sdhci_bus(struct sdhci *, unsigned bits, bool high_speed);
/* A command without data: its first response word, failed when any of the
 * errors bits shows in it. */
bool sdhci_command(struct sdhci *, unsigned index, uint32_t argument, unsigned type,
                   uint32_t errors, uint32_t *response);
/* A command the card may leave unanswered: 1 answered, its first response
 * word given for the caller to judge; 0 unanswered or garbled (a timeout,
 * CRC, end bit or index error on the command line), with the command and
 * data lines reset for the next command; -1 any other failure, which stops
 * the host. */
int sdhci_ask(struct sdhci *, unsigned index, uint32_t argument, unsigned type, uint32_t *response);
/* A command moving blocks of size bytes (at most 512, as many as count)
 * through the buffer port. */
bool sdhci_data(struct sdhci *, unsigned index, uint32_t argument, uint32_t errors, uint8_t *data,
                unsigned size, unsigned count, bool write);
/* A register of the controller, its wrapper or GCC, read or written 1, 2 or
 * 4 bytes wide, ordered against the accesses around it. */
uint32_t sdhci_read(uintptr_t address, unsigned width);
void sdhci_write(uintptr_t address, uint32_t value, unsigned width);
#endif
