#ifndef SOS2_GTA7LITE_H
#define SOS2_GTA7LITE_H
#include "battery.h"
#include "fdt.h"

/* Checks LK's handoff. Sets has_framebuffer once LK's scanout is in RAM and
 * clear of the boot data, so a later error can be displayed there. Returns
 * NULL or what is wrong. */
const char *gta7lite_handoff(struct platform_devices *d, uint64_t kernel_start, uint64_t kernel_end,
                             uint64_t dtb_address);
/* Restarts the countdown of TOPRGU's watchdog, which LK leaves running. */
void gta7lite_watchdog_feed(void);
/* The MT6357 PMIC through MT6765's PMIC wrapper (pmic.c), which LK leaves
 * running: false when the wrapper or PMIC is not as expected, and then
 * nothing below reads it. */
bool gta7lite_pmic_init(void);
/* Once per press of the power key, debounced. */
bool gta7lite_power_key_pressed(void);
/* The PMIC's RTC as Unix seconds: Android keeps it in UTC. */
bool gta7lite_rtc_read(uint32_t *seconds);
/* The battery's level, estimated from its voltage, and whether the MT6370
 * charges it, as LK left the charger configured. */
bool gta7lite_battery_read(struct battery_view *out);
/* An MT6357 register read; and a register's bits of mask set to value's,
 * the rest kept (a plain write for a mask of 0xffff): the audio codec, its
 * jack detection and clock buffer (audio.c), the CPUs' supplies
 * (cpu_clock.c). */
bool gta7lite_pmic_read(uint16_t address, uint16_t *value);
bool gta7lite_pmic_update(uint16_t address, uint16_t value, uint16_t mask);
/* A volume key (keys.c): +1 for Volume Up, -1 for Volume Down, once per
 * press and repeated while held; 0 otherwise. */
int gta7lite_volume_key(void);

/* The volume of the output in use (audio.c), from 0, silent, to
 * GTA7LITE_VOLUME_STEPS, after stepping it by step; and whether that output
 * is headphones in the jack, which turns the speakers off. */
enum { GTA7LITE_VOLUME_STEPS = 15 };

unsigned gta7lite_audio_volume(int step);
bool gta7lite_audio_headphones(void);

/* I2C (i2c.c): one register written, or read, of the device at a 7-bit
 * address. I2C5 is as LK left it; I2C3, whose registers for the AP are its
 * channel 0x100 in (the stock DT's ch_offset_default), needs its init. */
enum { GTA7LITE_I2C3 = 0x1100f100, GTA7LITE_I2C5 = 0x11016000 };

bool gta7lite_i2c_transfer(uintptr_t bus, uint8_t device, uint8_t reg, bool write, uint8_t *value);
void gta7lite_i2c3_init(void);
/* The CPU clocks (cpu_clock.c): MediaTek's top operating point, the supply
 * raised first, or LK's clocks back, the supply lowered last. False when the
 * clocks or supply are not as LK or the last change left them, or a supply
 * change does not confirm; the clocks are not changed again then. */
bool gta7lite_cpu_clock(bool fast);
/* The clocks' governor, from the main loop with process_user_us(): fast
 * while pages use a tenth of a CPU, LK's clocks after a second below. */
void gta7lite_cpu_clock_poll(uint64_t user_us);
/* The MT6370 (mt6370.c). The backlight's enable: off turns it off when LK
 * left it on, on restores it. The charger's state: 0 ready, 1 charging,
 * 2 done, 3 fault, or -1 when the chip does not answer. */
void gta7lite_backlight(bool on);
int gta7lite_charge_state(void);
/* Standby's panel (display.c): the backlight off and the panel asleep with
 * the overlay layers off, then awake and lit again. */
void gta7lite_display_standby(bool standby);
/* Standby's CPU power-down through MediaTek's ATF (cpu_power.c). On the CPU
 * itself: PSCI's CPU_OFF, which returns only when refused. On CPU 0: cut
 * the power of a CPU PSCI reports off, true once cut; and CPU_ON at entry
 * for a CPU cut first, true once started. */
void gta7lite_cpu_off(void);
bool gta7lite_cpu_cut(uint32_t mpidr);
bool gta7lite_cpu_on(uint32_t mpidr, uintptr_t entry);
/* Restarting into Samsung's download mode: whether LK's boot-params buffer
 * is there, and the reboot reason that asks for it set (or cleared again). */
bool gta7lite_download_mode_available(void);
bool gta7lite_download_mode(bool on);
/* Starts the touch screen (touch.c) once the MMU maps its devices and the
 * runtime files are in: SPI1 and its clock, the controller's ID and, sent
 * in about a second while the watchdog is fed, the firmware for the panel LK
 * found (lcm_name), then the latch for its interrupt line. Touches map onto
 * a width x height screen. Returns NULL or why touch is unavailable. */
const char *gta7lite_touch_init(const char *panel, uint32_t width, uint32_t height);
/* A pointer event from the report the controller signalled, read once its
 * interrupt's edge has latched (polled: the line raises no interrupt). The
 * first finger down is followed until it lifts; it is released when its
 * reports stop, and the gesture cancelled when they keep failing their
 * checks. False when there is nothing new. */
bool gta7lite_touch_poll(struct pointer_event *event);
/* Standby, which cancels the pointer itself: entering forgets the finger,
 * leaving drops edges latched meanwhile. The controller keeps running. */
void gta7lite_touch_standby(bool active);
/* The USB port as a fastboot device for the computer it is plugged into
 * (usb.c). Init powers and resets the PHY and controller, still unseen by
 * the host: NULL, or why the port is unavailable; later calls give the same
 * answer. Connect turns on the D+ pull-up once polls follow continually, and
 * the host enumerates the device. Poll services bus resets, control
 * requests and one fastboot command (fastboot.c). */
const char *gta7lite_usb_init(void);
void gta7lite_usb_connect(void);
void gta7lite_usb_poll(void);
/* For fastboot.c, on the bulk IN endpoint: send queues one packet, shorter
 * than the endpoint's so that it ends its transfer, and drain waits, briefly,
 * for the host to take it. False once the host stops reading or resets the
 * link: what was queued is dropped. */
bool gta7lite_usb_send(const void *data, uint32_t size);
void gta7lite_usb_drain(void);
/* A download's data from the bulk OUT endpoint: exactly size bytes, or false
 * once the host stops sending or resets the link. */
bool gta7lite_usb_read(void *data, uint32_t size);
/* VUSB33, the USB PHY's 3.3 V (pmic.c): on, and its output confirmed. */
bool gta7lite_usb_supply(void);
/* The tablet's fastboot commands and variables (fastboot.c) on the shared
 * protocol (kernel/usb/fastboot.c), before the host sees the link. */
void gta7lite_fastboot_init(void);

/* Wi-Fi. The connectivity subsystem's memory and Wi-Fi's DMA pool, where
 * Linux places the device tree's consys and wifi reservations (16 MiB-
 * aligned, below 0xC0000000); connsys.c keeps them out of every later
 * allocation, from gta7lite_handoff. */
enum {
    GTA7LITE_CONNSYS_EMI = 0xbf000000u,
    GTA7LITE_CONNSYS_EMI_SIZE = 0x440000u,
    GTA7LITE_WIFI_POOL = 0xbe000000u,
    GTA7LITE_WIFI_POOL_SIZE = 0x300000u
};

bool gta7lite_connsys_reserve(struct platform_devices *d, uint64_t kernel_start,
                              uint64_t kernel_end, uint64_t dtb, uint64_t dtb_size);
bool gta7lite_connsys_reserved(void);
/* The subsystem started up to RF calibration (connsys.c): 0, or why not.
 * yield runs between steps. */
const char *gta7lite_connsys_start(void (*yield)(void));
/* Its supplies (pmic.c): VCN18 and VCN33, then VCN28 for its crystal. */
bool gta7lite_connsys_supplies(void);
bool gta7lite_connsys_crystal_supply(void);
/* The radio of the shared Wi-Fi device (wifi.c, include/wifi_device.h). */
struct net_device;
void gta7lite_wifi_init(struct net_device *);
#endif
