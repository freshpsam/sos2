# Nexus 5

Target: LG Nexus 5 (**hammerhead**, D820, Qualcomm MSM8974 with four Krait
400 cores and 2 GB) with its stock bootloader (HHZ20h), unlocked. The ARM32
system [installs](#installing) into the boot partition and starts with the
phone, keeping its data in the cache partition, or starts from the computer
with `fastboot boot` as a RAM session, whose data clears when it restarts.
The desktop draws on the phone's screen and responds to touch, with a buzz
for buttons and keys; it joins open and WPA2 networks over [Wi-Fi](#wi-fi),
where pages load on all four cores at 1.5 GHz; the header shows the
battery; the power key's standby turns off the backlight, the panel and the
radio; and its USB port is a fastboot link to the computer. Packaging only
writes files on the computer; it never touches the phone.

## Building

```sh
make fetch && make engine                      # once
make nexus5 NEXUS5_STOCK=build/nexus5-stock    # build/nexus5/boot.img and more
```

`boot.img` is an Android boot v0 image (2 KiB pages) for the stock
bootloader (tools/package_nexus5.py): the kernel, linked at 0x8000, with a
zImage-shaped header and the device tree appended (boards/nexus5/boot.dts,
which the bootloader takes for its `qcom,msm-id` of 126, 150, 0x20002), and
the resources, engine included, as the Redmi 2's SORZ ramdisk, which the
kernel expands at boot (22 MB, 77 MB expanded). `boot.img.json` describes
the image: its addresses, sizes and hashes. As on the Redmi 2, the images
build in no apps: `make nexus5` writes them to `build/apps` as the Redmi 2's
ARM32 packages, and the installed system and its update to
`build/nexus5-install` ([Installing](#installing)). `NEXUS5_STOCK=folder`
names a folder with the Wi-Fi firmware ([below](#wi-fi)); without it the
phone has no Wi-Fi.
`NEXUS5_JSC=options` passes JavaScriptCore options to pages, as `REDMI2_JSC`
does on the Redmi 2.

## Starting

From the stock bootloader's fastboot (Power and Volume Down from off, or
`adb reboot bootloader` from Android):

```sh
fastboot boot build/nexus5/boot.img
```

The stock bootloader's fastboot and SOS2's link (below) are both fastboot
devices, so with both possible on the bus, name one with `-s`. The phone's
splash gives way to SOS2 within a second of the jump; the desktop takes
input about 25 s later, of which expanding the resources takes about 17 s.
A RAM session never touches the eMMC, so it can alternate with an installed
system, or with Android.

## Installing

`make nexus5` writes the installed system to `build/nexus5-install`, and
`make publish` puts it on [GitHub Pages](https://freshpsam.github.io/sos2/)
with the other devices' (`PUBLISH=nexus5` publishes the Nexus 5 alone):

- `flash-boot.img`: the whole system, as the RAM image holds it, with
  `sos2.storage=installed` in its command line.
- `sos2-nexus5.update`: the same image after a 4 KiB header, for the
  [updater](#updates-and-apps).
- `install.json`: the release, the files' sizes and SHA-256s, and these
  commands.

The image (22.7 MB) fits the boot partition (22 MiB, 23.1 MB) whole, because
SOS2 keeps English alone ([Architecture](architecture.md)). First keep
Android's boot image: from a RAM session, read the boot partition over the
USB link (15.6 s; nothing is written):

```sh
fastboot -s nexus5 oem stage-boot
fastboot -s nexus5 get_staged build/nexus5-stock/boot.img   # the whole 22 MiB
```

Then check `flash-boot.img`'s SHA-256 against `install.json` and, from the
stock bootloader's fastboot:

```sh
fastboot flash boot flash-boot.img
fastboot reboot
```

The stock bootloader then starts SOS2 whenever the phone starts, as
`fastboot boot` does, and SOS2 keeps web pages', apps' and the desktop's data
in the cache partition: 1 MiB left unused, the
[app store](redmi2.md#apps) (60 MiB) and the data volume (639 MiB of the
700 MiB; include/system_store.h), which it sets up at its first start in
0.24 s, whatever the partition held. It finds the `boot` and `cache`
partitions by name in a GPT it validates and writes only inside them;
recovery, system, userdata and the other partitions stay, and when the eMMC
fails the phone runs as a RAM session.

Android comes back with its boot image and an empty cache: Android 6 mounts
`/cache` as ext4 with `errors=panic`, which SOS2's data is not. The stock
bootloader formats it itself (its cache is ext4, 700 MiB), which erases
SOS2's app store and data volume:

```sh
fastboot flash boot build/nexus5-stock/boot.img
fastboot format cache
```

`make clean` deletes `build/`, so keep a copy of the backup elsewhere.

## Updates and apps

The installed system updates itself as the Redmi 2's does
([updates](redmi2.md#updates), [apps](redmi2.md#apps)), with the same ARM32
updater, which picks the device's update by the board the kernel names
(`lge-hammerhead`): **UPDATES → CHECK FOR UPDATES** reads
`https://freshpsam.github.io/sos2/nexus5/sos2-nexus5.update` and the ARM32
apps' `apps/index.txt`, the Redmi 2's; **INSTALL SYSTEM** downloads the boot
image, about 23 MB, which the kernel checks against the shape the stock
bootloader starts and writes into the boot partition, its first sector last;
**RESTART** starts it. A write interrupted before its end (about a minute of
writing and reading back) leaves no image to start, and the stock bootloader
stops in its fastboot, where flashing `flash-boot.img` finishes the job. A
RAM session installs nothing.

Tried on the phone: an update from one published release to the next over
Wi-Fi (the download about 115 s, at about 0.2 MB/s; writing about 40 s;
reading back about 20 s), the Calculator's install from the app list, and
the restart, after which the saved network joined again by itself. That
download ran before the driver read the chip's glommed frames
(2026-11-21), when about one received frame in ten was lost under load.

## USB link

While SOS2 runs, its USB port is a fastboot device for the computer it is
plugged into (boards/nexus5/usb.c and fastboot.c, with the protocol the
tablet shares in kernel/usb/fastboot.c), with fastboot's generic IDs
(18d1:d00d) and the serial number `nexus5`. It comes up once the desktop
is drawn, and stays up in a panic; when it cannot start, the desktop says
why (USB UNAVAILABLE). While a system update writes the boot partition it
neither restarts the phone nor reads the partition; after a panic it always
restarts, and reads the eMMC only if no service job was left waiting.

```sh
fastboot -s nexus5 getvar all
fastboot -s nexus5 oem stage-boot         # then get_staged: the boot partition
fastboot -s nexus5 reboot-bootloader      # the stock bootloader's fastboot
fastboot -s nexus5 reboot
```

With `reboot-bootloader` and `fastboot boot`, or `fastboot flash boot`, a
new image needs no hands on the phone.

## Wi-Fi

The phone joins open and WPA2 networks on 2.4 GHz from its Wi-Fi screen, and
pages load over them. Its chip, a Broadcom BCM4339, runs a firmware the
image carries: the stock system's, with the board's calibration, copied from
the phone's Android once (USB debugging on):

```sh
mkdir -p build/nexus5-stock/wifi-firmware
adb pull /system/vendor/firmware/fw_bcmdhd.bin build/nexus5-stock/wifi-firmware/
adb pull /system/etc/wifi/bcmdhd.cal build/nexus5-stock/wifi-firmware/
```

Android 6.0.1 (M4B30Z) has firmware 6.37.32.RC23.34.43. The packager checks
both files and adds about 0.3 MB to the image. `NEXUS5_WIFI_PROFILE=file`, a
profile made by `tools/redmi2_wifi.py` (open or WPA2;
[provisioning](redmi2.md#provisioning)), has the phone join that network at
boot, as on the other devices; the image then carries the network's key, so
it stays private. Otherwise a RAM session forgets its network when it
restarts; the installed system keeps the networks it joined in its data
volume.

While joined, the firmware saves power itself (PM_FAST: the radio sleeps
between beacons while the link is idle, as Android runs this chip). In
standby the station leaves its network, as on the other devices, and the
firmware then takes its radio down (`WLC_DOWN`), with nothing polling the
chip until standby ends; the saved network is joined again about 5 s after
the screen comes back on.

Tried: one WPA2-PSK router on channel 11, its key exchange, DHCP, the
clock from the network, pages, idle minutes, and standby and back. Not
yet: 5 GHz (the station scans channels 1 to 11), WPA3 and protected
management frames (the firmware has neither).

## Loader and hardware

- **Loader:** the stock bootloader puts the kernel at 0x8000, its copy of
  the device tree at 0x02700000, completed with RAM, the command line and the
  ramdisk, and the ramdisk at 0x02900000. It enters the kernel in SVC mode
  with the MMU and data cache off, the instruction cache on and alignment
  checks on, which the kernel turns off (the engine relies on unaligned
  access).
- **Memory:** 2 GiB in two banks, at 0 and 0x40000000. ARM32 user space
  (0x10000000-0x40000000) covers most of the first, so the kernel keeps its
  own memory below 0x10000000 or in the second bank, where the resources
  expand and the browser's memory lies. Reserved: the firmware's hole
  (0x07B00000-0x0FF00000, which holds the splash), the USB link's buffers
  (0x02600000) and LG's area at the start of the second bank.
- **CPU:** four Krait 400 cores (ARMv7, without LPAE or virtualization);
  the generic timer (19.2 MHz) through the QGIC2 at 0xF9000000. With the MMU
  off nothing is execute-never, and Krait fetched instructions ahead along
  predicted paths into memory TrustZone guards, which answered with a
  watchdog reset (`bootreason=wdog_reset` in Android's command line
  afterwards); which builds did it depended on their code layout. So the
  kernel's entry (arch/arm/boot.S), once BSS is clear, turns translation on
  behind tables of the kernel alone (arch/arm/boot_cache.c): its text
  executable, everything else Device memory and execute-never, which is
  never fetched ahead. Only then do the instruction cache and branch
  prediction come on, which brings the main loop about 0.4 s sooner than
  waiting for the resources' expansion. The data cache is on only while the
  resources expand, behind the boot cache's ranges added to the same
  tables; the kernel's own tables replace them when the protected processes
  start (vm_init). Under
  load the cores read a spurious interrupt ID (1023) a few times in 10 s,
  which the scheduler takes as a preemption point (arch/arm/timer.c).
- **Other cores:** started as the stock kernel starts them
  (boards/nexus5/smp.c, Google's platsmp.c and krait-regulator.c): their
  entry through TrustZone's legacy SCM boot address (the call shared with
  the Redmi 2, boards/qcom/scm.c), each core's own clock while it is still
  off, then its power through its ACC at 0xF9088000 + n 0x10000: the head
  switch on in segments with the LDO bypassed, the CPU rail's four phases
  (a command the L2's SAW2 sends to the PMIC), then the clamp and reset
  released. The firmware starts a core at its entry at once; an SGI sent as
  Linux sends one would wait until the core first took interrupts. In
  standby they wait in WFE, still clocked.
- **CPU clocks:** each core and the L2 have their own HFPLL (L x 19.2 MHz)
  and a mux in an L2 indirect register (CPMR, CP15's indirect window), which
  parks a clock on GPLL0's 300 MHz while its HFPLL restarts
  (boards/nexus5/cpu_clock.c, the stock acpuclock-krait.c). The bootloader
  leaves CPU 0 at 960 MHz and the L2 at 729.6 MHz, the CPU rail (PM8841 S5)
  at 900 mV, the memory rail (S1) at 1.05 V and the logic rail (S2) at
  1.03 V, its NORMAL level. This phone's part is speed bin 1, voltage class
  2 (fuses at 0xFC4B80B0). Its table pairs each core rate with an L2 rate,
  and L2 rates above 1036.8 MHz need the logic rail above NORMAL, which
  only the RPM sets (the stock kernel's SMD messages); a core's own HFPLL
  needs NORMAL up to L 104, and the memory rail is at the table's highest
  already. So at boot the L2 moves to 1036.8 MHz and every core to
  1497.6 MHz, which the table gives 910 mV: the CPU rail rises 10 mV first,
  written to PM8841 S5 over SPMI, read back, given its slew time and
  confirmed ready (without that the cores take 1190.4 MHz, 865 mV). That
  shortens the boot by about 11 s. In standby CPU 0 and the L2 wait on
  300 MHz with their HFPLLs off.
- **Display:** a command-mode DSI panel, 1080x1920. The bootloader's splash
  is MDP pipe RGB0, RGB888 at 0x0D200000, and becomes the screen, drawn at
  full resolution. A command-mode panel shows a new picture only when the
  display processor sends a frame: the kernel starts the splash's control
  path (CTL0's start register) when the screen has changed, at most every
  16 ms, and otherwise once a second, once DSI0 shows the last frame sent
  (the done flag it latches for the display processor's frames, or its
  status) or after 250 ms (boards/nexus5/display.c). Standby
  turns the backlight off, sends a black frame, then puts the panel to
  sleep as the stock driver's off commands do (display off, 50 ms, sleep
  in, 100 ms) and wakes it before frames go again (sleep out, 120 ms,
  display on): DCS short writes through DSI0's command DMA, which the
  bootloader leaves set for software triggers in high speed (packet
  address at DSI0 + 0x48, length + 0x4C, trigger + 0x90, done in bit 0 of
  + 0x110, as the stock mdss_dsi_cmd_dma_tx sends them).
- **Backlight:** a TI LM3630 at 0x38 on BLSP2 QUP5 (0xF9967000, GPIO 83 and
  84) at 100 kHz, enabled by GPIO 90 (boards/nexus5/backlight.c). The
  bootloader leaves it lit (brightness 32); standby clears its control
  register and drops the enable, and the wake raises the enable and sets
  the stock driver's values again (boost 0x30, feedback from bank A, full
  scale 0x12, bank A on with both strings) and the brightness, as
  lm3630_bl.c does.
- **Battery:** a MAX17048 fuel gauge at 0x36 and a BQ24192 charger at 0x6b
  on BLSP1 QUP1 (0xF9923000, GPIO 2 and 3) at 100 kHz
  (boards/nexus5/battery.c), both only read: the gauge's cell voltage
  (validating a live reading) and its level, which this phone counts in
  half the datasheet's steps (it read 193.2 % at 4.29 V; Linux's device tree
  says `maxim,double-soc`), scaled from its empty point (1.0 %) to its full
  one (97.0 %) as the stock driver scales it; the charger's power-good and
  charging bits. The bootloader leaves the charger configured (4.352 V,
  500 mA from USB) with its watchdog off, so nothing needs writing.
- **Vibrator:** a motor driver enabled by GPIO 60 and steered by a PWM on
  GPIO 27 (its gp1_clk function): MMCC's CAMSS GP1 clock as an M/N:D counter,
  19.2 MHz / 4 over a period of 127, about 37.8 kHz, whose duty is the drive
  (half none, more forward, less braking), as the stock
  msm8974_pwm_vibrator.c runs it (boards/nexus5/vibrator.c). Its supply,
  PM8941 L19 at 3.3 V, feeds nothing else; the stock kernel asks the RPM
  for it, here it is switched on directly over SPMI, which the arbiter
  allows. A tap drives it at full forward duty for 15 ms, then brakes it
  for 15 ms, both timed by the main loop's haptics polls
  (kernel/protection/haptics.c). The I2C buses' clocks and pins come up
  through one helper
  (boards/nexus5/i2c.c).
- **Touch:** a Synaptics S3350 (product ID PLG218) at 0x70 on BLSP1 QUP2
  (0xF9924000, GPIO 6 and 7) at 355 kHz, as Linux runs it; its attention
  line is GPIO 5 and its reset GPIO 8 (boards/nexus5/touch.c). The
  bootloader leaves its supplies on and its reset released; the kernel turns
  on the bus's clock and pins. It speaks RMI4: the kernel finds its device
  control (F01) and 2D sensor (F12) in its page description table, the
  sensor's report layout in its register descriptors and its size (1080x1920,
  the screen's) in its tuning register. The main loop reads the attention
  line; when it is low, the interrupt status and then the first objects'
  positions. The first finger down is the pointer until it lifts. Standby
  puts the sensor to sleep. The I2C driver (boards/qcom/qup_i2c.c) is the
  Redmi 2's, as both chips have the same QUP controller.
- **PMIC:** the PM8941 and PM8841, through the SPMI arbiter's channel 0
  (boards/nexus5/pmic.c; a register's slave in bits 19:16 of its address):
  the power key, which toggles standby; the RTC, the clock; L19 for the
  vibrator; and restarts: a warm reset through PS_HOLD, with the restart
  reason in IMEM (0x77665500 asks the bootloader for fastboot).
- **USB:** the ChipIdea controller at 0xF9A55000 (boards/nexus5/usb.c),
  polled by the main loop, set up as the bootloader's driver sets it up, its
  queue heads and buffers in uncached RAM at 0x02600000. A transfer is over
  once the controller has also retired it (its ENDPTSTAT bit clear): the
  descriptor's token is written back first, and a prime made before then is
  lost (about one reply in 60 never went out).
- **Wi-Fi's bus:** the BCM4339 on SDCC2 (0xF98A4000, its SDHCI at
  0xF98A4900), SDIO with 4 bits at 50 MHz (boards/nexus5/sdio.c), the chip
  powered by TLMM GPIO 26 with its 32 kHz sleep clock out of PM8941 GPIO16.
  The controller, Qualcomm's SDHCI 3.0 behind its wrapper, starts cold as
  Linux's sdhci-msm starts it: HC mode, the I/O pads switched to 1.8 V,
  each bus power or voltage change confirmed on the wrapper's power-control
  registers, and the card's clock set at GCC (400 kHz, then 50 MHz in high
  speed). At 400 kHz a status clear lands late: a command sent right after
  one finished with nothing latched, so commands wait 40 us after a clear
  then, as the stock kernel has its older controllers do.
- **Wi-Fi's chip:** brought up as Linux's brcmfmac does
  (boards/nexus5/bcm4339.c): its backplane through function 1's 32 KiB
  window (a 4339 reads 0x4335 from revision 2), its cores from the
  enumeration ROM, the firmware written into the ARM CR4's 768 KiB of RAM
  with the calibration at its end, the CPU started; then SDPCM frames on
  function 2 carry BCDC ioctls, events and Ethernet frames, polled through
  the SDIO core's interrupt status. The firmware starts in 0.24 s. The chip
  gloms the frames it sends, bus:txglom or not: a descriptor lists the
  slots of the superframe that follows (four 1472-byte slots under a ping
  load), each holding a frame of its own.
- **Wi-Fi's radio:** the firmware is FullMAC: it scans, authenticates and
  associates by itself, where SOS2's station (kernel/wifi) builds and parses
  those frames. So the radio (boards/nexus5/wifi.c) tells the station what
  the firmware does through the station's FullMAC calls
  (include/wifi_station.h), as brcmfmac runs it with a host supplicant: each
  channel's scan is an escan whose results become the beacons they
  describe; the station's authentication is answered at once; its
  association request becomes the firmware's join, with the station's RSN
  element, and the station is associated when the firmware reports the
  network joined; WPA2's handshake runs in SOS2 over EAPOL data frames, its
  keys go to the firmware; data moves as Ethernet, converted to and from
  802.11 data frames; a link the firmware loses, or a deauthentication it
  reports, disconnects the station; and while the firmware holds the link,
  the station's probes of a quiet network count as the network heard.
  Frames and calls reach the station in the order they came. Without
  protected management frames in the firmware, the station never negotiates
  them and never chooses WPA3 (wifi_rsn_pmf in kernel/wifi/wifi_auth.c).
- **eMMC:** a 16 GB card (15028 MiB, EXT_CSD revision 6: eMMC 4.5) on SDCC1
  (0xF9824000, its SDHCI at 0xF9824900), with 8 bits at 50 MHz in high
  speed (boards/nexus5/emmc.c), on the SD host code that Wi-Fi's bus and the
  Redmi 2's eMMC share (boards/qcom/sdhci.c). The bootloader leaves the
  controller in legacy
  mode with its SDHCI registers as after a reset, so the card starts cold, as
  Linux's MMC core starts an eMMC: CMD0, CMD1 until it is ready with sector
  addressing, its address, selected, its EXT_CSD, then high-speed timing, the
  clock and the 8-bit bus, checked by reading the EXT_CSD again. Its I/O is
  fixed at 1.8 V, so unlike SDCC2 the wrapper asks for no signalling
  confirmation (MCI_GENERICS' switchable bit). Transfers run in PIO. A read
  of several sectors is one CMD18 of up to 256, which the host ends with
  its own CMD12 (auto CMD12): the boot partition reads in about 4.5 s over
  the USB link, against 15.6 s a sector at a time. One sector is CMD17: a
  CMD18 for one, never stopped, left the card sending, and the next command
  timed out. Writes move a sector at a time, about 0.6 MB/s.
- **Randomness:** jitterentropy's CPU timing jitter seeding an HMAC_DRBG, as
  on the other devices. At the bootloader's 960 MHz 32 bytes of it took
  about 1.2 s, so about 4 s with CPU 0 and the L2 parked at 300 MHz in
  standby, and a read gets 8 s here.
- **Not yet:** sound (the WCD9320 codec on SLIMbus behind the audio DSP,
  whose firmware, its messaging and the codec's setup would all have to
  run first); the GPU (an Adreno 330, the Redmi 2's family); CPU levels
  above 1497.6 MHz (the RPM, and a temperature watch); the cores'
  power-down in standby (TrustZone's power collapse through each core's
  SPM, as on the Redmi 2); the Wi-Fi chip's own sleep in standby
  (brcmfmac's KSO, with the HT clock released); the panel's sleep and wake
  commands run in steps rather than holding CPU 0's loop (about 150 ms
  entering standby, 120 ms leaving); and a received frame read in one
  CMD53 rather than its header first. The vibrator's buzz and the
  backlight's and panel's return are checked on their registers, not yet by
  hand.
