#include "redmi2_idle_devices.h"
#include "msm8916_i2c.h"
#include "platform.h"

/* Devices SOS2 does not use, left as the boot chain leaves them, put into
 * their lowest-power state once at boot, as their Linux drivers leave them
 * when idle:
 * - the motion sensor, an InvenSense MPU6880 (vendor device tree: "invn,
 *   mpu6050" at 0x68 on BLSP1 QUP2, GPIO 6/7; supply L17, the panel's). The
 *   MPU-6500 family powers up awake with its gyroscope and accelerometer
 *   running, a few mA (PWR_MGMT_1 0x01): it gets SLEEP (0x40), as
 *   inv_mpu6050 sets it when idle, after WHO_AM_I names an InvenSense part,
 *   and again after each standby, whose whole-chip sleeps turn L17 off. Its
 *   bus then stops its clock. The light sensor and magnetometer on that bus
 *   power up in standby;
 * - the power domains of the video codec, JPEG encoder and camera (Venus,
 *   JPEG, VFE), switched off as Linux switches off any unused domain at boot
 *   (genpd_power_off_unused): their clocks first, then the waits gdsc.c sets
 *   and the collapse request, unless the hardware controls the domain;
 * - the USB controller and its ULPI PHY, which lk2nd's fastboot leaves
 *   running: the PHY reset, the controller set up again as lk2nd's udc_init
 *   sets it up and stopped, the PHY suspended (PORTSC PHCD, then the ULPI
 *   STP line) and the clocks off, as msm_otg_suspend leaves them with no
 *   cable (the SMB1360 charger senses VBUS on its own); again as each
 *   standby begins;
 * - unused devices' pins, set as the vendor kernel's sleep states leave
 *   them (msm8916-pinctrl.dtsi and the WT88047's board files): the cameras'
 *   resets, standbys and clocks, the codec's PDM link, the amplifiers' and
 *   key backlight's enables, the flash, the LED controller's I2C, the
 *   magnetometer's reset, the IMU's interrupt, the SD card's detect and
 *   pads, and the boot chain's debug UART.
 * Addresses: Linux gcc-msm8916.c, pinctrl-msm8916.c (blsp_i2c2 is function
 * 3), msm8916.dtsi; lk2nd's hsusb.h for the ChipIdea controller. */
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

#define CLOCK_OFF 0x80000000u

enum {
    BLSP_VOTE = 0x01845004,
    BLSP_AHB = 1u << 10,
    BLSP_AHB_CBCR = 0x01801008,
    QUP2_CMD = 0x01803000,
    QUP2_CFG = 0x01803004,
    QUP2_CBCR = 0x01803010,
    XO_SOURCE_MASK = 0x1f | (7 << 8) | (3 << 12) | (1 << 20),
    TLMM = 0x01000000,
    GPIO_STRIDE = 0x1000,
    GPIO_CFG_MASK = 0x3ff,
    I2C2_FUNCTION = 3 << 2,
    IMU = 0x68,
    WHO_AM_I = 0x75,
    PWR_MGMT_1 = 0x6b,
    SLEEP = 0x40,
    INT_PIN_CFG = 0x37,
    INT_ENABLE = 0x38,
    WAIT_US = 10000,
};
static struct msm8916_i2c sensors = {.base = 0x078b6000};

static bool wait_bits_for(uintptr_t address, uint32_t mask, uint32_t value, uint32_t limit) {
    uint64_t start = time_us();
    while ((read32(address) & mask) != value)
        if (time_us() - start >= limit) return false;
    return true;
}

static bool wait_bits(uintptr_t address, uint32_t mask, uint32_t value) {
    return wait_bits_for(address, mask, value, WAIT_US);
}

static bool branch(uintptr_t cbcr, bool on) {
    write32(cbcr, on ? read32(cbcr) | 1 : read32(cbcr) & ~1u);
    return wait_bits(cbcr, CLOCK_OFF, on ? 0 : CLOCK_OFF);
}

/* As battery.c brings up QUP4: the shared BLSP1 AHB vote, the QUP's I2C
 * clock from the 19.2 MHz crystal, its branch, then the pins. */
static bool sensor_bus(void) {
    write32(BLSP_VOTE, read32(BLSP_VOTE) | BLSP_AHB);
    if (!(read32(BLSP_VOTE) & BLSP_AHB) || !wait_bits(BLSP_AHB_CBCR, CLOCK_OFF, 0)) return false;
    write32(QUP2_CFG, (read32(QUP2_CFG) & ~(uint32_t)XO_SOURCE_MASK) | 1);
    write32(QUP2_CMD, read32(QUP2_CMD) | 1);
    if (!wait_bits(QUP2_CMD, 1, 0) || !branch(QUP2_CBCR, true)) return false;
    for (unsigned pin = 6; pin <= 7; ++pin) {
        uintptr_t cfg = TLMM + pin * GPIO_STRIDE;
        write32(cfg, (read32(cfg) & ~(uint32_t)GPIO_CFG_MASK) | I2C2_FUNCTION);
        if ((read32(cfg) & GPIO_CFG_MASK) != I2C2_FUNCTION) return false;
    }
    return msm8916_i2c_bus_init(&sensors);
}

static bool imu_sleep(void) {
    if (!sensor_bus()) return false;
    uint8_t id = 0, power = 0;
    bool ok = msm8916_i2c_bus_read(&sensors, IMU, WHO_AM_I, &id, 1);
    /* InvenSense MPU-6000/6050 (0x68), 6500 (0x70), 9250 (0x71), 9255 (0x73),
     * 6515 (0x74), 6880 (0x78). */
    if (ok && id != 0x68 && id != 0x70 && id != 0x71 && id != 0x73 && id != 0x74 && id != 0x78)
        ok = false;
    else if (ok) {
        /* Its interrupt as at reset, push-pull active high and off, so the
         * line rests low on the pin's pull-down whatever a previous OS set;
         * sleep is tried even if this does not take. */
        (void)msm8916_i2c_bus_write(&sensors, IMU, INT_ENABLE, 0);
        (void)msm8916_i2c_bus_write(&sensors, IMU, INT_PIN_CFG, 0);
        if (msm8916_i2c_bus_read(&sensors, IMU, PWR_MGMT_1, &power, 1) && !(power & SLEEP))
            ok = msm8916_i2c_bus_write(&sensors, IMU, PWR_MGMT_1, SLEEP) &&
                 msm8916_i2c_bus_read(&sensors, IMU, PWR_MGMT_1, &power, 1) && (power & SLEEP);
        else ok = (power & SLEEP) != 0;
    }
    (void)branch(QUP2_CBCR, false);
    return ok;
}

/* gcc-msm8916.c's venus_gdsc, jpeg_gdsc and vfe_gdsc, with the clocks in
 * each domain. */
static const struct domain {
    uint32_t gdscr, branch[7];
} domains[] = {
    {0x0184c018, {0x0184c01c, 0x0184c020, 0x0184c024}}, /* Venus */
    {0x0185701c, {0x01857020, 0x01857024, 0x01857028}}, /* JPEG */
    {0x01858034,
     {0x01858038, 0x0185803c, 0x01858040, 0x01858044, 0x01858048, 0x01858050,
      0x0185600c}}, /* VFE */
};

enum {
    PWR_ON = 1u << 31,
    HW_CONTROL = 1u << 1,
    SW_COLLAPSE = 1,
    GDSC_FIELDS = 0x00fff007,
    GDSC_WAITS = 0x00282000,
    COLLAPSE_US = 1500
};

static void domains_off(void) {
    for (unsigned i = 0; i < sizeof(domains) / sizeof(domains[0]); ++i) {
        const struct domain *d = &domains[i];
        uint32_t gdscr = read32(d->gdscr);
        if (!(gdscr & PWR_ON) || (gdscr & HW_CONTROL)) continue;
        bool off = true;
        for (unsigned b = 0; off && b < 7 && d->branch[b]; ++b) off = branch(d->branch[b], false);
        if (off) {
            write32(d->gdscr, (gdscr & ~(uint32_t)GDSC_FIELDS) | GDSC_WAITS | SW_COLLAPSE);
            (void)wait_bits_for(d->gdscr, PWR_ON, 0, COLLAPSE_US);
        }
    }
}

/* The USB block (ChipIdea registers at 0x078d9000, lk2nd's hsusb.h), its
 * PHY's reset and its GCC clocks: the system clock at 80 MHz, GPLL0 / 10, as
 * lk2nd's hsusb_clock_init sets it. */
enum {
    USB = 0x078d9000,
    SBUSCFG = 0x090,
    AHB_MODE = 0x098,
    GENCONFIG_2 = 0x0a0,
    USBCMD = 0x140,
    USBINTR = 0x148,
    DEVICEADDR = 0x154,
    ULPI_VIEWPORT = 0x170,
    PORTSC = 0x184,
    USBMODE = 0x1a8,
    CMD_RESET = 2,
    CMD_ITC8 = 8u << 16,
    CMD_SESSION_VALID = 1u << 25,
    CMD_ASYNC_INTR = 1u << 29,
    CMD_ULPI_STP = 1u << 30,
    PORT_PHCD = 1u << 23,
    PORT_ULPI = 2u << 30,
    GEN2_SESSION_VALID = 1u << 7,
    MODE_DEVICE = 2,
    AHB_HPROT = 8,
    ULPI_WAKEUP = 1u << 31,
    ULPI_RUN = 1u << 30,
    ULPI_WRITE = 1u << 29,
    ULPI_MISC_A_SET = 0x97,
    ULPI_VBUS_EXTERNAL = 3,
    USB_PHY_BCR = 0x01841028,
    USB_SYSTEM_CBCR = 0x01841004,
    USB_AHB_CBCR = 0x01841008,
    USB_SYSTEM_CMD = 0x01841010,
    USB_SYSTEM_CFG = 0x01841014,
    RCG_MASK = 0x371f,
    RCG_GPLL0_DIV10 = 0x113,
    RCG_UPDATE = 1,
    GPLL0_STATUS = 0x0182101c,
    GPLL0_ACTIVE = 1u << 17,
    USB_US = 100000,
    PHY_RESET_US = 1000
};


/* A ULPI register write through the viewport, its wake-up first as Linux's
 * ci_ulpi_write sends it; only the write itself must finish. */
static bool ulpi_write(uint8_t reg, uint8_t value) {
    write32(USB + ULPI_VIEWPORT, ULPI_WAKEUP | ULPI_WRITE);
    (void)wait_bits(USB + ULPI_VIEWPORT, ULPI_WAKEUP, 0);
    write32(USB + ULPI_VIEWPORT, ULPI_RUN | ULPI_WRITE | (uint32_t)reg << 16 | value);
    return wait_bits(USB + ULPI_VIEWPORT, ULPI_RUN, 0);
}

/* lk2nd's fastboot ends with udc_stop, which clears the PHY's external VBUS
 * input and resets the controller, with the PHY running. A PHY suspended
 * straight from there is deaf to ULPI and draws about 3 mA in standby; a
 * controller reset does not wake it, a PHY reset does. So the PHY is reset
 * first, through its block reset
 * (GCC_USB2A_PHY_BCR, its "phy" reset in Linux's msm8916.dtsi), woken if the
 * controller still holds it suspended (msm_otg_resume), and the controller
 * reset and set up as lk2nd's udc_init sets it up: the ULPI PHY selected,
 * VBUS taken as valid through the PHY's external input, bus access mode,
 * device mode, interrupts off, stopped. Then the suspend, tried whatever
 * failed before it. Without GPLL0 the controller cannot be reached, so the
 * PHY is left alone. */
static void usb_off(void) {
    if (!(read32(GPLL0_STATUS) & GPLL0_ACTIVE)) return;
    write32(USB_PHY_BCR, read32(USB_PHY_BCR) | 1u);
    delay_us(PHY_RESET_US);
    write32(USB_PHY_BCR, read32(USB_PHY_BCR) & ~1u);
    delay_us(PHY_RESET_US);
    /* Registers need both clocks, so they run until it is done. */
    bool clocks = true;
    if ((read32(USB_SYSTEM_CFG) & RCG_MASK) != RCG_GPLL0_DIV10) {
        write32(USB_SYSTEM_CFG, (read32(USB_SYSTEM_CFG) & ~(uint32_t)RCG_MASK) | RCG_GPLL0_DIV10);
        write32(USB_SYSTEM_CMD, read32(USB_SYSTEM_CMD) | RCG_UPDATE);
        clocks = wait_bits(USB_SYSTEM_CMD, RCG_UPDATE, 0);
    }
    if (clocks && branch(USB_AHB_CBCR, true) && branch(USB_SYSTEM_CBCR, true)) {
        write32(USB + USBCMD, read32(USB + USBCMD) & ~(uint32_t)(CMD_ASYNC_INTR | CMD_ULPI_STP));
        bool awake = true;
        if (read32(USB + PORTSC) & PORT_PHCD) {
            write32(USB + PORTSC, read32(USB + PORTSC) & ~(uint32_t)PORT_PHCD);
            awake = wait_bits_for(USB + PORTSC, PORT_PHCD, 0, USB_US);
        }
        if (awake) {
            write32(USB + USBCMD, CMD_ITC8 | CMD_RESET);
            awake = wait_bits_for(USB + USBCMD, CMD_RESET, 0, USB_US);
        }
        if (awake) {
            write32(USB + PORTSC, PORT_ULPI);
            (void)ulpi_write(ULPI_MISC_A_SET, ULPI_VBUS_EXTERNAL);
            write32(USB + GENCONFIG_2, read32(USB + GENCONFIG_2) | GEN2_SESSION_VALID);
            write32(USB + USBCMD, read32(USB + USBCMD) | CMD_SESSION_VALID);
            write32(USB + SBUSCFG, 0);
            write32(USB + AHB_MODE, AHB_HPROT);
            write32(USB + USBMODE, MODE_DEVICE);
            write32(USB + USBINTR, 0);
            write32(USB + DEVICEADDR, 0);
        }
        write32(USB + PORTSC, read32(USB + PORTSC) | PORT_PHCD);
        if (wait_bits_for(USB + PORTSC, PORT_PHCD, PORT_PHCD, USB_US))
            write32(USB + USBCMD, read32(USB + USBCMD) | CMD_ASYNC_INTR | CMD_ULPI_STP);
    }
    (void)branch(USB_SYSTEM_CBCR, false);
    (void)branch(USB_AHB_CBCR, false);
}

/* TLMM GPIO_CFG (function 5:2, pull 1:0 none, down, keeper, up; drive 8:6,
 * mA/2 - 1; output enable 9) and IN_OUT (output 1). */
enum {
    TLMM_CFG_MASK = 0x3ff,
    OE = 1u << 9,
    IO_OUT = 2,
    SDC2_PADS = 0x01109000,
    SDC2_MASK = 0xdfff,
    SDC2_SLEEP = 0x1e00
};

static const struct pin {
    uint8_t gpio;
    uint16_t cfg;
} pins[] = {
    {26,0x001},{27,0x001},                                     /* camera clocks: input, pulled down */
    {28,0x201},{33,0x201},{34,0x201},{35,0x201},{120,0x201},   /* camera resets, standbys, focus: low */
    {29,0x004},{30,0x004},                                     /* camera I2C (CCI) */
    {63,0x005},{64,0x005},{65,0x005},{66,0x005},{67,0x005},{68,0x005}, /* codec PDM link: pulled down */
    {8,0x200},{117,0x200},{119,0x200},                         /* amplifier and key backlight enables: low */
    {31,0x001},{32,0x001},                                     /* flash */
    {22,0x008},{23,0x008},                                     /* LED controller I2C (QUP6) */
    {36,0x001},{115,0x001},                                    /* magnetometer reset, IMU interrupt */
    {38,0x001},                                                /* SD card detect */
    {4,0x001},{5,0x001},                                       /* debug UART */
};

static void pins_idle(void) {
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        uintptr_t cfg = TLMM + pins[i].gpio * GPIO_STRIDE;
        if (pins[i].cfg & OE)
            write32(cfg + 4, read32(cfg + 4) & ~(uint32_t)IO_OUT); /* the level before the enable */
        write32(cfg, (read32(cfg) & ~(uint32_t)TLMM_CFG_MASK) | pins[i].cfg);
    }
    write32(SDC2_PADS, (read32(SDC2_PADS) & ~(uint32_t)SDC2_MASK) | SDC2_SLEEP);
}

void redmi2_idle_devices_init(void) {
    domains_off();
    usb_off();
    pins_idle();
    (void)imu_sleep();
}

void redmi2_idle_devices_standby(void) { usb_off(); }

/* After a standby the motion sensor may have lost its supply (L17 goes off
 * while the chip sleeps, rails.c): it then powers up running, and answers
 * 100 ms after its supply rises (the MPU-6500 family's start-up time for
 * register access). Sleep is tried 150 ms after the wake, then every 250 ms,
 * four times at most; one already asleep only has its state read. */
enum { IMU_START_US = 150000, IMU_RETRY_US = 250000, IMU_TRIES = 4 };

static uint64_t imu_due;
static unsigned imu_tries;

void redmi2_idle_devices_wake(void) {
    imu_due = time_us() + IMU_START_US;
    imu_tries = IMU_TRIES;
}

void redmi2_idle_devices_poll(void) {
    if (!imu_tries || time_us() < imu_due) return;
    --imu_tries;
    if (imu_sleep()) imu_tries = 0;
    else imu_due = time_us() + IMU_RETRY_US;
}
