#ifndef SOS2_NEXUS5_H
#define SOS2_NEXUS5_H
#include "fdt.h"
#include <stdbool.h>
#include <stdint.h>

/* The Nexus 5 (LG hammerhead, MSM8974 with Krait CPUs), started by its stock
 * bootloader: its drivers (boards/nexus5) and their kernel hooks. */

/* The splash the bootloader leaves, which becomes the screen: 1080x1920
 * RGB888, rows 3240 bytes apart (board.c, display.c). */
enum { NEXUS5_WIDTH = 1080, NEXUS5_HEIGHT = 1920, NEXUS5_STRIDE = NEXUS5_WIDTH * 3 };

/* What the drivers share (board.c): TLMM GPIO n's configuration register
 * (function, drive, pull, output enable; its input and output follow at
 * + 4), a register store completed before what follows, and a wait of up
 * to us microseconds for bits of a register to read set, or clear. */
enum { NEXUS5_TLMM = 0xfd511000 };

static inline uintptr_t nexus5_gpio(unsigned n) { return NEXUS5_TLMM + 0x10u * n; }

void nexus5_store(uintptr_t address, uint32_t value);
bool nexus5_wait_bits(uintptr_t address, uint32_t bits, bool set, uint64_t us);

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
/* Sends a frame when the screen changed (damage) and the last is at least a
 * frame's time old, or once a second regardless. */
void nexus5_display_damage(void);
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
 * full drive, or stops the motor; brake drives it backwards after a pulse
 * and says for how long (0 when it cannot), haptics.c then stopping it. */
bool nexus5_vibrator_init(void);
uint32_t nexus5_vibrator_brake(void);
bool nexus5_vibrator_run(bool on);

/* The cores' and L2's clocks (cpu_clock.c), on CPU 0: init moves CPU 0 and
 * the L2 from the bootloader's rates to the floor of the stock table's
 * points for this part (false: left as they were); start clocks a core that
 * is still off at the current point; poll, each loop turn with the
 * processes' user time, raises the cores while pages work and lowers them
 * after, within what their temperature allows; standby puts the cores at
 * the floor and parks CPU 0 and the L2 on 300 MHz, and back. */
bool nexus5_cpu_clock_init(void);
bool nexus5_cpu_clock_start(unsigned cpu);
void nexus5_cpu_clock_poll(uint64_t user_us);
void nexus5_cpu_clock_standby(bool on);
/* A touch: interactive, so sustained work keeps the faster clocks. */
void nexus5_cpu_clock_touch(void);
/* The cores' temperature sensors (thermal.c): init takes their calibration
 * from the fuses once the sensors are ready, waiting up to wait_us for
 * them; temperature gives the hottest core's, in degrees. */
bool nexus5_thermal_init(uint64_t wait_us);
bool nexus5_cpu_temperature(int *degrees);
/* The RPM (rpm.c): the logic rail's corner while the apps processor runs
 * (rpm-regulator-smd.h's: 5 normal, 7 super turbo), acknowledged. A
 * shared memory item's address and size, 0 when it is absent; one
 * allocated in the shared heap (or the one already there, if its size
 * matches), 0 when it cannot be. */
bool nexus5_rpm_logic_corner(unsigned corner);
uintptr_t nexus5_smem_item(unsigned id, uint32_t *size);
uintptr_t nexus5_smem_alloc(unsigned id, uint32_t size);

/* Sound (docs/nexus5.md#sound): the loudspeaker through the audio DSP and
 * the WCD9320 codec.
 *
 * The audio DSP (adsp.c): its firmware read from Android's system
 * partition, then authenticated and started by TrustZone, a step at a time
 * for the service worker: true once it is done (*error then NULL, or why
 * the DSP did not start), false while steps remain or after a failure
 * (*error set). One of the channels the DSP opened to this processor, by
 * name, opening (smd.c: poll it until it is open); false while the DSP has
 * not listed it. */
bool nexus5_adsp_load_step(const char **error);
struct smd_channel;
bool nexus5_adsp_channel(const char *name, struct smd_channel *out, uint64_t now);

/* The DSP's APR services (apr.c): a packet with its header filled in for
 * service svc at port; a command and its answer, a basic result or a
 * response whose payload is copied to out: 0 when it succeeded, negative
 * when it went unsent or unanswered, 1 when the DSP refused it; events go
 * to the handler. Poll keeps the channel read: false once it failed. */
bool nexus5_apr_poll(void);
bool nexus5_apr_send(uint8_t *packet, unsigned size, uint8_t svc, uint16_t port, uint32_t token,
                     uint32_t opcode);
int nexus5_apr_command(uint8_t *packet, unsigned size, uint8_t svc, uint16_t port, uint32_t token,
                       uint32_t opcode, uint32_t response, void *out, unsigned out_size);
void nexus5_apr_events(void (*handler)(const uint8_t *packet, unsigned size));

/* The DSP's QMI services (qmi.c), over its IPC router link: a request's
 * TLVs to a service, answered within wait_us: 0 when the response's result
 * is success, else negative (no link, no service, no answer) or the QMI
 * error with bit 16 set; whether the DSP has listed a service. */
int nexus5_qmi_request(uint32_t service, uint32_t instance, uint16_t message, const uint8_t *tlvs,
                       unsigned length, uint64_t wait_us);
void nexus5_qmi_poll(void);
bool nexus5_qmi_has(uint32_t service, uint32_t instance);

/* SLIMbus as a satellite of the DSP's manager (slimbus.c): start has the
 * DSP power the bus and announces this processor (NULL, or why not); a
 * device's logical address by its enumeration address; a byte value
 * element (a codec register) read or changed; a device's port connected as
 * a channel's sink, the channel defined and activated with its properties,
 * or removed; stop asks the bus off. */
const char *nexus5_slimbus_start(void);
bool nexus5_slimbus_address(const uint8_t enumeration[6], uint8_t *la);
bool nexus5_slimbus_write(uint8_t la, uint16_t offset, uint8_t value);
bool nexus5_slimbus_read(uint8_t la, uint16_t offset, uint8_t *value);
bool nexus5_slimbus_connect_sink(uint8_t la, uint8_t port, uint8_t channel);
bool nexus5_slimbus_define(uint8_t la, const uint8_t properties[4], uint8_t channel);
bool nexus5_slimbus_remove(uint8_t la, uint8_t channel);
bool nexus5_slimbus_stop(void);

/* Playback through the DSP (q6audio.c): the AFE's SLIMbus port on the
 * codec's channel, the stream and its route, the shared memory mapped,
 * running, buffers written (done gets each one's token back), closed. */
bool nexus5_q6_afe_start(uint8_t channel);
bool nexus5_q6_afe_stop(void);
bool nexus5_q6_open(void);
bool nexus5_q6_map(uint32_t address, uint32_t size);
bool nexus5_q6_run(void (*done)(uint32_t token));
bool nexus5_q6_write(uint32_t address, uint32_t size, uint32_t token);
bool nexus5_q6_close(void);

/* The codec (wcd9320.c) and the whole loudspeaker path: on, the bus
 * powered, the codec up with its speaker route, the DSP's port started and
 * the codec's SLIMbus port on the shared channel (NULL, or why not); off,
 * the reverse. */
const char *nexus5_speaker(bool on);

/* The loudspeaker's level (audio.c): the volume step after a key's step
 * (+1, -1, or 0 to read it); the volume keys (keys.c): +1 for a Volume Up
 * step, -1 for Volume Down, else 0. */
unsigned nexus5_audio_volume(int step);
int nexus5_volume_key(void);

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

/* The USB port as a fastboot device (usb.c, with the shared protocol in
 * kernel/usb/fastboot.c and the phone's commands in fastboot.c). init sets
 * the controller up (NULL, or why not); connect lets the host see it, from
 * when poll runs often. rescue keeps the link up in a panic, started then
 * if it was not yet: false if the controller does not start. send is one
 * transfer to the host. fastboot_init registers the phone's commands. */
const char *nexus5_usb_init(void);
void nexus5_usb_connect(void);
void nexus5_usb_poll(void);
bool nexus5_usb_rescue(void);
bool nexus5_usb_send(const void *data, uint32_t length);
void nexus5_fastboot_init(void);
#endif
