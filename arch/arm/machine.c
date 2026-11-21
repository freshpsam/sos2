#include "arm_machine.h"
#include "platform.h"
#include "user_abi.h"
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_rtc.h"
#endif
#ifdef SOS2_BOARD_GTA7LITE
#include "gta7lite.h"
#endif
#ifdef SOS2_BOARD_NEXUS5
#include "nexus5.h"
#endif

static struct arm_machine machine;

static bool device(uintptr_t start, uint32_t size) {
    if (!start || !size || start > UINT32_MAX - (size - 1) ||
        (start<USER_END &&(uint64_t)start + size> USER_BASE) ||
        machine.device_count >= sizeof(machine.devices) / sizeof(machine.devices[0]))
        return false;
    machine.devices[machine.device_count++] = (struct arm_mapping){start, size};
    return true;
}

const struct arm_machine *arm_machine_get(void) { return &machine; }

bool arm_machine_init(const struct platform_devices *d) {
    machine = (struct arm_machine){0};
    if (!d) return false;
#ifdef SOS2_BOARD_REDMI2
    if (!d->redmi2_wt88047 || !d->has_framebuffer) return false;
    /* MSM8916 qgic2 and virtual-timer PPI 4 (INTID 20), from msm8916.dtsi.
     * These are the same MMIO windows used by the existing polled phone drivers.
     * No QEMU peripheral or secure firmware reservation is mapped.
     * https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/msm8916.dtsi
     */
    machine.timer = (struct arm_timer_route){0x0b000000, 0x0b002000, 20, true};
    static const struct arm_mapping phone[] = {
        {0x01000000,0x7a000}, /* TLMM GPIO 0-121: drivers' pins, unused devices' pins in their sleep states */
        {0x01109000,0x1000}, /* TLMM SDC2 pad control: the unused SD card's pads */
        {0x00060000,0x8000}, /* RPM message RAM, explicit SMEM auxiliary region */
        {0x01908000,0x1000}, /* TCSR hardware mutex 3: SMEM allocation */
        {0x0a202000,0x4000}, /* WCNSS DXE and CCU */
        {0x0a21b000,0x3000}, /* Pronto v2 PMU */
        {0x0b011000,0x1000}, /* APPS mailbox signals to RPM/WCNSS, CPU clock mux */
        {0x0b098000,0x1000}, {0x0b0a8000,0x1000}, {0x0b0b8000,0x1000}, /* CPU1..3 ACC */
        {0x0b089000,0x1000}, {0x0b099000,0x1000}, {0x0b0a9000,0x1000}, {0x0b0b9000,0x1000}, /* CPU0..3 SAW2: standby power-down */
        {0x0b020000,0x2000}, /* Memory-mapped timer control and frame 0: CPU 0's wake-up */
        {0x0b012000,0x1000}, /* L2 SAW2: whole-chip sleep */
        {0x0190c000,0x1000}, /* TCSR mutex 7: the secure monitor's power-collapse hand-off */
        {0x004a3000,0x1000}, /* MPM2 sleep counter (32 kHz) */
        {0x004ab000,0x1000}, /* MPM2 PS_HOLD: dropped to restart */
        {0x01803000,0x1000}, {0x078b6000,0x1000}, /* QUP2 clock and QUP2: the motion sensor */
        {0x0184c000,0x1000}, {0x01856000,0x3000}, /* Venus, JPEG and VFE power domains (with CPP's micro AHB): off when unused */
        {0x078d9000,0x1000}, {0x01841000,0x1000}, /* USB controller and its clocks: suspended at boot */
        {0x86300000,0x100000}, /* Shared memory ABI; supervisor-only, uncached */
        {0x01801000,0x1000}, {0x01805000,0x2000}, {0x01845000,0x1000}, /* GCC, QUP4/5 */
        {0x01816000,0x1000}, {0x01821000,0x1000}, /* crypto clocks and GPLL0 status */
        {0x01826000,0x1000}, /* SNOC QoS clock gate used by stock WCNSS startup */
        {0x01812000,0x1000}, {0x01831000,0x1000}, {0x01859000,0x1000}, /* GPU clocks / OXILI power */
        {0x01c00000,0x20000}, /* Adreno 306 register aperture */
        {0x01f09000,0x2000}, /* GPU SMMU USER/PRIV context banks; no bypass */
        {0x0200f000,0x1000}, /* SPMI arbiter version and channel table */
        {0x0200a000,0x1000}, /* SPMI arbiter interrupt ownership */
        {0x03800000,0x200000}, /* SPMI arbiter interrupt accumulators: the power key */
        {0x02400000,0x400000}, {0x02c00000,0x400000}, /* SPMI write/read channels */
        {0x078b8000,0x2000}, /* QUP4 battery and QUP5 touch I2C */
        {0x07824000,0x1000}, /* SDCC1 core and SDHCI: inherited eMMC controller */
        {0x01842000,0x1000}, /* GCC SDCC1 clock source: the eMMC's bus rate */
        {0x01a98000,0x1000}, /* DSI0 controller, PLL and PHY: panel sleep, pipeline off */
        {0x0184d000,0x1000}, /* GCC MDSS clocks and power domain */
        {0x01a01000,0x2000}, {0x01a6b000,0x1000}, /* MDP top and CTL0, INTF1 */
    };
    for (unsigned i = 0; i < sizeof(phone) / sizeof(phone[0]); ++i)
        if (!device(phone[i].start, phone[i].size)) return false;
#elif defined(SOS2_BOARD_NEXUS5)
    if (!d->nexus5_hammerhead || !d->has_framebuffer) return false;
    /* MSM8974's QGIC2 and the virtual timer's PPI 4 (INTID 20), from Google's
     * hammerhead msm8974.dtsi and Linux's qcom-msm8974.dtsi. */
    machine.timer = (struct arm_timer_route){0xf9000000, 0xf9002000, 20, true};
    static const struct arm_mapping phone[] = {
        {0xfd8c2000, 0x2000},  /* MMCC: the display's clocks, the vibrator's PWM */
        {0xfd900000, 0x23000}, /* MDSS: MDP 1.2, the splash's CTL and DSI0 (display.c) */
        {0xfc4cf000, 0x1000},  /* SPMI arbiter core, channel 0: PM8941 (pmic.c) */
        {0xf9a55000, 0x1000},  /* ChipIdea USB controller: the fastboot link (usb.c) */
        {0x02600000, 0x10000}, /* its queue heads and buffers, RAM kept uncached */
        {0xfc400000, 0x2000},  /* GCC: the USB controller's and I2C buses' clocks */
        {0xf9923000, 0x2000},  /* BLSP1 QUP1, QUP2: the battery's and touch screen's I2C */
        {0xf9967000, 0x1000},  /* BLSP2 QUP5: the backlight's I2C bus */
        {0xfd511000, 0x1000},  /* TLMM GPIO 0-145: the touch screen's and Wi-Fi's pins */
        {0xfd512000, 0x1000},  /* TLMM SDC pad control: SDC2's, Wi-Fi's bus */
        {0xf98a4000, 0x1000},  /* SDCC2 and its SDHCI: the Wi-Fi chip's SDIO (sdio.c) */
        {0xf9824000, 0x1000},  /* SDCC1 and its SDHCI: the eMMC (emmc.c) */
        {0xfe805000, 0x1000},  /* IMEM: the restart reason the bootloader reads */
        {0xf9010000, 0x10000}, /* APCS: its clock control, the L2's SAW2 and HFPLL */
        {0xf9088000, 0x40000}, /* each CPU's ACC, SAW2 and HFPLL (n at 0x10000 n) */
        {0xfc4b8000, 0x1000},  /* QFPROM: the CPUs' speed and voltage bins */
        {0xfc4ab000, 0x1000},  /* MPM2 PS_HOLD: dropped to restart */
    };
    for (unsigned i = 0; i < sizeof(phone) / sizeof(phone[0]); ++i)
        if (!device(phone[i].start, phone[i].size)) return false;
#elif defined(SOS2_BOARD_GTA7LITE)
    /* MT6765's GICv3 (Samsung's mt6765.dts): the distributor and the
     * redistributors' 2 MiB window, one 128 KiB frame per CPU; the virtual
     * timer's PPI 11 (INTID 27). TOPRGU, whose watchdog LK leaves running. */
    if (!d->gicv3) return false;
    machine.timer = (struct arm_timer_route){.distributor = 0x0c000000,
                                             .interrupt = 27,
                                             .gicv3 = true,
                                             .redistributor = 0x0c100000,
                                             .redistributor_size = 0x200000};
    if (!device(0x10007000, 0x1000)) return false;
    /* The touch screen (boards/gta7lite/touch.c): topckgen's and infracfg's
     * SPI clock controls, the GPIO pins, the external interrupt controller
     * that latches the touch interrupt, and SPI1. */
    if (!device(0x10000000, 0x2000) || !device(0x10005000, 0x1000) || !device(0x1000b000, 0x1000) ||
        !device(0x11010000, 0x1000))
        return false;
    /* The PMIC wrapper (boards/gta7lite/pmic.c) and the eMMC's MSDC0
     * (boards/gta7lite/emmc.c). */
    if (!device(0x1000d000, 0x1000) || !device(0x11230000, 0x10000)) return false;
    /* The USB controller and its PHY (boards/gta7lite/usb.c). */
    if (!device(0x11200000, 0x1000) || !device(0x11cc0000, 0x1000)) return false;
    /* Wi-Fi (boards/gta7lite/connsys.c and wifi_radio.c): the radio chip's pin
     * driving, SPM's power domains, BTIF, the connectivity subsystem with
     * Wi-Fi's registers, then its memory and Wi-Fi's DMA pool, which
     * gta7lite_handoff kept out of every allocation. */
    if (!device(0x10002000, 0x1000) || !device(0x10006000, 0x1000) || !device(0x1100c000, 0x1000) ||
        !device(0x18000000, 0x100000))
        return false;
    if (gta7lite_connsys_reserved() && (!device(GTA7LITE_CONNSYS_EMI, GTA7LITE_CONNSYS_EMI_SIZE) ||
                                        !device(GTA7LITE_WIFI_POOL, GTA7LITE_WIFI_POOL_SIZE)))
        return false;
    /* Standby (boards/gta7lite/mt6370.c and display.c): I2C5, where the
     * MT6370 charger and backlight driver answer; OVL0, OVL0_2L and DSI0. */
    if (!device(0x11016000, 0x1000) || !device(0x1400b000, 0x2000) || !device(0x14014000, 0x1000))
        return false;
    /* The speakers (boards/gta7lite/audio.c): the audio front end with the
     * part of its SRAM it plays from, and I2C3 for the amplifiers; the
     * keypad, whose Volume Up keys.c reads. */
    if (!device(0x11220000, 0x8000) || !device(0x1100f000, 0x1000) || !device(0x10010000, 0x1000))
        return false;
    /* The CPU clocks' PLLs in APMIXEDSYS (boards/gta7lite/cpu_clock.c). */
    if (!device(0x1000c000, 0x1000)) return false;
    /* MediaTek's boot-params buffer in SRAM, for restarting into download
     * mode (boards/gta7lite/board.c). */
    if (d->has_ram_console) {
        uint32_t start = d->ram_console_start & ~0xfffu;
        uint32_t end = (d->ram_console_start + d->ram_console_size + 0xfffu) & ~0xfffu;
        if (!device(start, end - start)) return false;
    }
    return device(machine.timer.distributor, 0x10000) &&
           device(machine.timer.redistributor, machine.timer.redistributor_size);
#else
    /* QEMU virt's GICv2 and the virtual timer's PPI 11 (INTID 27). */
    machine.timer = (struct arm_timer_route){0x08000000, 0x08010000, 27, false};
    machine.rtc = 0x09010000;
    if (!device(d->fwcfg, 0x1000) || !device(machine.rtc, 0x1000)) return false;
    if (d->power_key && !device(d->gpio, 0x1000)) return false;
    if (d->virtio_count > MAX_VIRTIO) return false;
    for (size_t i = 0; i < d->virtio_count; ++i)
        if (!device(d->virtio[i], 0x200)) return false;
#endif
    return device(machine.timer.distributor, 0x1000) && device(machine.timer.cpu, 0x2000);
}

enum arm_rtc_kind arm_rtc_kind(void) {
#ifdef SOS2_PHONE
    return ARM_RTC_PMIC;
#else
    return ARM_RTC_UNIX;
#endif
}

bool arm_rtc_read(uint32_t *seconds) {
    if (!seconds) return false;
#ifdef SOS2_BOARD_REDMI2
    return redmi2_rtc_read(seconds);
#elif defined(SOS2_BOARD_NEXUS5)
    /* PM8941's counter, which stock Android never sets: an unknown epoch. */
    return nexus5_rtc_read(seconds);
#elif defined(SOS2_BOARD_GTA7LITE)
    /* The MT6357's calendar, which Android keeps in UTC: Unix seconds. */
    return gta7lite_rtc_read(seconds);
#else
    if (!machine.rtc) return false;
    *seconds = mmio_read32(machine.rtc);
    return true;
#endif
}
