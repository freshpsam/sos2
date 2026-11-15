#include "redmi2_chip_sleep.h"
#include "redmi2_cpu_power.h"
#include "redmi2_cpu_clock.h"
#include "redmi2_wifi.h"
#include "redmi2_display.h"
#include "platform.h"

/* Whole-chip sleep, as the vendor kernel suspends an MSM8916 (lpm-levels.c
 * lpm_suspend_enter at cluster level l2-pc, msm8916-pm.dtsi): in standby's
 * quiet polls, with the three secondaries powered down, the display off and
 * the RPM's clock scaling on (Wi-Fi's start-up), the L2 cache goes off with
 * CPU 0 (cpu_power.c) and its sequence hands the chip to the RPM.
 * The RPM applies the sleep set (Wi-Fi's, protected_devices.c, releasing the
 * crystal and core rails Wi-Fi's start-up voted for) and, as every master
 * allows, turns the crystal off (XO shutdown) and lowers the core rails as
 * well (VDD minimum), then wakes the chip through the MPM:
 * - the vMPM in RPM message RAM (0x601d0, mpm-of.c msm_mpm_set): the wake
 *   time as an absolute count of the 19.2 MHz counter, pin 62 enabled and
 *   level high (the SPMI arbiter's interrupt for this processor: the power
 *   key), no edges, status cleared; then APCS IPC bit 1 (0x0b011008).
 * Across each sleep the 19.2 MHz counter is checked against
 * the MPM's 32 kHz sleep counter (0x004a3000): if it lagged, the crystal
 * stopped it, and whole-chip sleep stops for the boot. The CPU clock's mux
 * is checked too (ramp_up_first_cpu). Ten sleeps in a row that the firmware
 * refuses stop it for that standby. An RPM request never answered keeps the
 * chip awake (the vendor never sleeps with one outstanding). */
static uint32_t read32(uintptr_t address) {
    uint32_t value = mmio_read32(address);
    barrier();
    return value;
}

static void write32(uintptr_t address, uint32_t value) {
    barrier();
    mmio_write32(address, value);
    barrier();
}

enum {
    VMPM = 0x000601d0,
    MPM_TIMER = 0x00,
    MPM_ENABLE = 0x08,
    MPM_FALLING = 0x10,
    MPM_RISING = 0x18,
    MPM_POLARITY = 0x20,
    MPM_STATUS = 0x28,
    KEY_PIN = 1u << 30,
    IPC = 0x0b011008,
    IPC_MPM = 2,
    SLEEP_COUNTER = 0x004a3000,
    SLEEP_HZ = 32768,
    COUNTER_HZ = 19200000,
    FALL_LIMIT = 10,
};

static bool ready, stopped, active; /* stopped: no more whole-chip sleep this boot */
static unsigned consecutive;        /* Sleeps the firmware refused in a row. */

/* A register written by another processor, read until two reads agree. */
static uint32_t settled(uintptr_t address) {
    uint32_t value = read32(address);
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t again = read32(address);
        if (again == value) break;
        value = again;
    }
    return value;
}

bool redmi2_chip_sleep_init(void) {
    ready = stopped = active = false;
    ready = redmi2_l2_power_init();
    return ready;
}

void redmi2_chip_sleep_standby(bool on) {
    consecutive = 0;
    active = on && ready && !stopped;
    if (active) redmi2_wifi_sleep_set();
}

bool redmi2_chip_sleep(uint32_t microseconds) {
    /* A display left running would lose its PHY's settings under it, and
     * without the RPM's clock scaling RAM would come back slow. */
    if (!active || stopped || consecutive >= FALL_LIMIT || !redmi2_display_off() ||
        !redmi2_wifi_clock_scaling() || !redmi2_wifi_rpm_idle() ||
        redmi2_wifi_sleep_set_state() == 0)
        return false;
    for (unsigned cpu = 1; cpu < 4; ++cpu)
        if (!redmi2_cpu_power_collapsed(cpu)) return false;
    uint64_t start = redmi2_cpu0_count();
    if (!start) return false;
    uint32_t clock = redmi2_cpu_clock_config(), sleep_start = settled(SLEEP_COUNTER);
    /* The MPM's wake-up for the RPM (msm_mpm_set), then its doorbell. */
    uint64_t wake = start + (uint64_t)microseconds * (COUNTER_HZ / 1000) / 1000;
    write32(VMPM + MPM_TIMER, (uint32_t)wake);
    write32(VMPM + MPM_TIMER + 4, (uint32_t)(wake >> 32));
    write32(VMPM + MPM_ENABLE, 0);
    write32(VMPM + MPM_ENABLE + 4, KEY_PIN);
    write32(VMPM + MPM_FALLING, 0);
    write32(VMPM + MPM_FALLING + 4, 0);
    write32(VMPM + MPM_RISING, 0);
    write32(VMPM + MPM_RISING + 4, 0);
    write32(VMPM + MPM_POLARITY, 0);
    write32(VMPM + MPM_POLARITY + 4, KEY_PIN);
    write32(VMPM + MPM_STATUS, 0);
    write32(VMPM + MPM_STATUS + 4, 0);
    write32(IPC, IPC_MPM);
    int result = redmi2_cpu0_deep_sleep(microseconds);
    if (result < 0) {
        stopped = true;
        return false;
    }
    if (!result) {
        /* The key's line came up as CPU 0 went down: the caller polls it now. */
        if (redmi2_cpu0_source_seen()) return true;
        ++consecutive;
        return false;
    }
    consecutive = 0;
    uint64_t counted = redmi2_cpu0_count() - start,
             expected = (uint64_t)(settled(SLEEP_COUNTER) - sleep_start) * COUNTER_HZ / SLEEP_HZ;
    if (counted + counted / 1000 + 2 * COUNTER_HZ / SLEEP_HZ < expected) stopped = true;
    redmi2_cpu_clock_reapply(clock);
    return true;
}
