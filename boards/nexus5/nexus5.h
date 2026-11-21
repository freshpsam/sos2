#ifndef SOS2_NEXUS5_H
#define SOS2_NEXUS5_H
#include "fdt.h"
#include <stdbool.h>
#include <stdint.h>

/* The Nexus 5 (LG hammerhead, MSM8974 with Krait CPUs), started by its stock
 * bootloader: its drivers (boards/nexus5) and their kernel hooks. */

/* Checks the bootloader's handoff (board.c): RAM, the kernel, the device
 * tree and the ramdisk, and takes the splash as the screen. NULL when all
 * is well, else why not; the screen is set whenever the splash was found,
 * so the message can show. Reads registers, writes none. */
const char *nexus5_handoff(struct platform_devices *d, uint64_t kernel_start, uint64_t kernel_end,
                           uint64_t dtb_address);

/* The command-mode panel shows a new picture only when the display processor
 * sends it a frame (display.c). */
/* The splash scanout the bootloader left: NULL with its address, or why it
 * cannot be used. */
const char *nexus5_display_handoff(uint32_t *scanout);
/* Sends a frame when the last is at least a frame's time old. */
void nexus5_display_poll(void);
/* Sends a frame of what the screen holds now, once the last has gone. */
void nexus5_display_flush(void);

/* Standby: one last frame (the black screen), then the panel asleep and no
 * frames until it ends, when the panel wakes first. */
void nexus5_display_standby(bool on);
/* The backlight (backlight.c): off for standby, then on again at the
 * bootloader's brightness. False when the controller did not take it. */
bool nexus5_backlight(bool on);
/* The devices the handoff described (board.c). */
const struct platform_devices *nexus5_devices(void);

/* The PMICs over the SPMI arbiter (pmic.c). A register, its slave in bits
 * 19:16 (the PM8941's are 0 and 1, the PM8841's 4 and 5): read, or written
 * and read back. */
bool nexus5_pmic_read(uint32_t address, uint8_t *value);
bool nexus5_pmic_write(uint32_t address, uint8_t value);
/* A press of the power key since the last call. */
bool nexus5_power_key_pressed(void);
/* The PMIC's real-time clock, in seconds since an unknown epoch. */
bool nexus5_rtc_read(uint32_t *seconds);
/* A restart, into the stock bootloader's fastboot or a normal start: the
 * reason in IMEM, the PMIC's PS_HOLD reset set to a warm reset, then
 * PS_HOLD dropped. */
_Noreturn void nexus5_restart(bool bootloader);

/* The touch screen (touch.c), once the MMU maps its devices: its bus's
 * clocks and pins, the controller (reset if it does not answer) and its
 * registers. Touches map onto a width x height screen. NULL, or why touch
 * is unavailable. Poll reads a report when the controller signals one and
 * follows the first finger down until it lifts; reports that keep failing
 * cancel the gesture. False when there is nothing new. Standby puts the
 * sensor to sleep and forgets the finger; waking drops what was pending. */
struct pointer_event;
const char *nexus5_touch_init(uint32_t width, uint32_t height);
bool nexus5_touch_poll(struct pointer_event *event);
void nexus5_touch_standby(bool on);

/* An I2C bus of BLSP1 or BLSP2 (i2c.c): its clocks on, its pins (the SDA
 * and SCL GPIOs) in their I2C function, then the controller (qup_i2c.c).
 * False if it did not start. */
struct qup_i2c;
bool nexus5_i2c_start(struct qup_i2c *bus, unsigned sda, unsigned scl);

/* The vibrator (vibrator.c) for the shared touch feedback (haptics.c): init
 * powers its supply and pins, false if it cannot; run starts a pulse at
 * full drive, or brakes the motor for 15 ms and stops it. */
bool nexus5_vibrator_init(void);
bool nexus5_vibrator_run(bool on);

/* The cores' and L2's clocks (cpu_clock.c), on CPU 0: init moves CPU 0 and
 * the L2 from the bootloader's rates to the faster ones the rails carry as
 * they are (false: left as they were); start clocks a core that is still
 * off at that rate; standby parks CPU 0 and the L2 on 300 MHz and back. */
bool nexus5_cpu_clock_init(void);
bool nexus5_cpu_clock_start(unsigned cpu);
void nexus5_cpu_clock_standby(bool on);

/* The other three cores (smp.c), for arch/arm/smp.c on CPU 0: prepare
 * gives the firmware their entry and returns how many cores there are (1
 * when they cannot start); release clocks and powers one up. */
unsigned nexus5_smp_prepare(uint32_t boot_mpidr, uintptr_t entry);
bool nexus5_smp_release(unsigned cpu);

/* The battery (battery.c), for the battery icon (kernel/protection/battery.c)
 * on the service worker: the MAX17048 gauge's level and the BQ24192
 * charger's state. False when a read failed. */
struct battery_view;
bool nexus5_battery_read(struct battery_view *out);

/* Wi-Fi (wifi.c): the Broadcom BCM4339 as the shared Wi-Fi device's radio.
 * power switches on the chip's supply and sleep clock: NULL, or why not. */
struct net_device;
void nexus5_wifi_init(struct net_device *out);
const char *nexus5_wifi_power(void);

/* The BCM4339's firmware (bcm4339.c), on the Wi-Fi service worker:
 * firmware says whether the image carries /firmware/brcm's firmware and
 * calibration; start loads them and waits for the firmware (NULL, or why
 * not), then events and Ethernet frames go to these functions from poll,
 * no more than room says the radio can take, and from any wait for an
 * ioctl's reply or for credit, regardless. ioctl and iovar return 0, a
 * negative firmware error, or -1000 when unanswered; a GET's answer replaces
 * buffer's (or value's) bytes. send takes an Ethernet frame. stop, after a
 * failure, refuses the chip from then on and wipes the buffers that carried
 * its keys. */
bool nexus5_bcm_firmware(void);
const char *nexus5_bcm_start(void (*event)(const uint8_t *message, unsigned length),
                             void (*frame)(const uint8_t *ethernet, unsigned length),
                             unsigned (*room)(void));
int nexus5_bcm_ioctl(unsigned command, bool set, void *buffer, unsigned length,
                     unsigned out_length);
int nexus5_bcm_iovar(const char *name, bool set, void *value, unsigned length);
int nexus5_bcm_iovar_int(const char *name, uint32_t value);
bool nexus5_bcm_send(const uint8_t *ethernet, unsigned length, unsigned priority);
bool nexus5_bcm_poll(void);
void nexus5_bcm_stop(void);

/* The chip's SDIO bus (sdio.c), on the Wi-Fi device's service worker. start
 * powers the chip and brings SDCC2 and the card up (4 bits, 50 MHz): NULL,
 * or why not; stop, after a failure, refuses the card from then on. byte is
 * CMD52; transfer is CMD53, whole blocks of the function's block size while
 * more than a block remains, then bytes; increment walks the address, else
 * it stays (a FIFO). function enables a function and waits up to wait_ms for
 * it. */
const char *nexus5_sdio_start(void);
void nexus5_sdio_stop(void);
bool nexus5_sdio_byte(unsigned function, uint32_t address, uint8_t *value, bool write);
bool nexus5_sdio_transfer(unsigned function, uint32_t address, void *data, unsigned size,
                          bool write, bool increment);
bool nexus5_sdio_block_size(unsigned function, unsigned size);
bool nexus5_sdio_function(unsigned function, bool on, unsigned wait_ms);

/* The eMMC (emmc.c, the kernel's emmc_card): idle unless the service worker
 * waits inside a transfer, when the main loop must leave the card alone. */
bool nexus5_emmc_idle(void);

/* The USB port as a fastboot device (usb.c, fastboot.c). init sets the
 * controller up (NULL, or why not); connect lets the host see it, from
 * when poll runs often. rescue keeps the link up in a panic, started then
 * if it was not yet: false if the controller does not start. fastboot_panic
 * tells the commands that the main loop and the service worker are gone. */
const char *nexus5_usb_init(void);
void nexus5_usb_connect(void);
void nexus5_usb_poll(void);
bool nexus5_usb_rescue(void);
bool nexus5_usb_send(const void *data, uint32_t length);
void nexus5_fastboot(const char *command, unsigned length);
void nexus5_fastboot_panic(void);
#endif
