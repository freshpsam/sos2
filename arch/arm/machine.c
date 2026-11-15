#include "arm_machine.h"
#include "platform.h"
#include "user_abi.h"
#ifdef SOS2_BOARD_REDMI2
#include "redmi2_rtc.h"
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
#else
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
#ifdef SOS2_BOARD_REDMI2
    return ARM_RTC_PMIC;
#else
    return ARM_RTC_UNIX;
#endif
}

bool arm_rtc_read(uint32_t *seconds) {
    if (!seconds) return false;
#ifdef SOS2_BOARD_REDMI2
    return redmi2_rtc_read(seconds);
#else
    if (!machine.rtc) return false;
    *seconds = mmio_read32(machine.rtc);
    return true;
#endif
}
