# Galaxy Tab A7 Lite

Target: Samsung Galaxy Tab A7 Lite Wi-Fi, **SM-T220** (gta7litewifi, MediaTek
MT8768WT, 3 GB), the AArch64 system's hardware. SOS2 starts on the tablet
from its installed system, with the engine, and responds to taps. Its
randomness, clock and battery icon work, and its data volume in the cache
partition (formatted and mounted at the first start) keeps data across
restarts. Its Wi-Fi joins a WPA2 network, over which pages load and the
[updater](#updates-and-apps) installs new releases and apps from GitHub
Pages, as on the Redmi 2. Its power key toggles standby, from which Wi-Fi
rejoins within seconds. Packaging only writes files on the computer; it never
touches the tablet.

## Building

```sh
make fetch && make ARCH=arm64 engine   # once
make tablet                            # build/gta7lite: Image.gz, resources.sorz
```

`GTA7LITE_STOCK=folder` names a folder with `boot.img` and `vbmeta.img` from
the tablet's stock firmware (T220XXSAEYE4: lz4-compressed in its AP file,
which `zstd -d` unpacks); with it the target also writes:

- `boot.img`: an Android header v2 image with the stock header's load
  addresses, command line and device tree table (Android's DT table format,
  holding one device tree), which LK completes at boot. It holds the kernel,
  gzip-compressed; the resources, engine included, as a SORZ ramdisk (about
  30 MB), which the kernel expands into free RAM at boot; and Samsung's
  `SEANDROIDENFORCE` trailer. The packager refuses an image larger than the
  boot partition, which the stock image fills up to its AVB footer.
- `boot.tar`: that image as odin4 flashes it.
- `vbmeta-disabled.tar`: the stock `vbmeta.img` with AVB verification
  disabled, which LK needs before it starts any boot image but Samsung's.
- `flash-boot.img`: the installed system, below.

The touch controller runs firmware that the kernel sends it at every boot:
the stock vendor partition's `/vendor/firmware/novatek_ts_*_fw.bin`, of which
the panel decides one, as in Samsung's driver (for `nt36523bh_qunchuang_inx`,
`novatek_ts_pid7221_fw.bin`). The vendor partition is in the AP file's
`super.img`. Put the files in `GTA7LITE_STOCK/touch-firmware/`, or name
another folder with `GTA7LITE_TOUCH_FIRMWARE`; the ramdisk then carries them
under `/firmware/novatek`. Like the Redmi 2's Wi-Fi firmware they stay out of
the repository. Without them the tablet has no touch, and its desktop says so.

Wi-Fi needs the stock vendor partition's connectivity firmware, also from
`/vendor/firmware`: `soc1_0_ram_mcu_1_1_hdr.bin`,
`soc1_0_ram_wifi_1_1_hdr.bin`, `soc1_0_ram_bt_1_1_hdr.bin`,
`soc1_0_patch_mcu_1_1_hdr.bin` and `WIFI_RAM_CODE_soc1_0_1_1.bin`. Put them
in `GTA7LITE_STOCK/wifi-firmware/`, or name another folder with
`GTA7LITE_WIFI_FIRMWARE`; the packager checks their headers and the ramdisk
carries them under `/firmware/connsys`. Without them the tablet has no Wi-Fi.
With `GTA7LITE_WIFI_PROFILE=file`, a profile made by `tools/redmi2_wifi.py`
([Redmi 2 Wi-Fi](redmi2-wifi.md)), the tablet joins that network at boot; the
image then carries the network's key, so it stays private.
`GTA7LITE_JSC=options` passes JavaScriptCore options to pages, as
`REDMI2_JSC` does on the phone
([packaging options](redmi2.md#packaging-options)).

As on the Redmi 2, the images build in no apps: `make tablet` writes them to
`build/apps-arm64` as AArch64 packages, and with `GTA7LITE_STOCK` the
installed system and its update to `build/gta7lite-install`
(`flash-boot.img`, `sos2-gta7lite.update` and `install.json`), which `make
publish` puts on GitHub Pages.

Keep the stock firmware in a separate directory from the build outputs.
Without `GTA7LITE_STOCK`, the packager writes only the kernel and ramdisk
and removes flash images whose checksums match its previous build manifest.
Older, untracked flash images may still be present in the output directory.

The boot partition holds 32 MiB (recovery 37 MiB, cache 200 MiB), of which
`boot.img` takes about 29 MB: a RAM session, whose data is gone at the next
start.

The installed system, `flash-boot.img`, is the same image and keeps its
data, as on the Redmi 2 (include/system_store.h, docs/redmi2.md): its command
line adds `sos2.storage=installed`. LK ends its own long command line with
the image's, and the kernel picks its `sos2.*` arguments out of it
(kernel/fdt.c). The cache partition holds 1 MiB left
unused, the app store (60 MiB) and the data volume (139 MiB), which SOS2 sets
up at its first start. No other partition is written, so Android's data
stays, but Android cannot start again until its cache holds an empty ext4
again: its fstab mounts `/cache` as ext4 with `errors=panic`, and the stock
boot image flashed back alone ends at "Can't load Android system". The stock
firmware's own empty cache image restores it: `cache.img.lz4` in the
HOME_CSC file, decompressed (`lz4 -d`) and unsparsed (`simg2img`) to the
partition's 200 MiB, then written with the stock boot image, which keeps
Android's data:

```sh
heimdall flash --boot boot.img --cache cache.img
```

That erases SOS2's app store and data volume. The RAM session (`boot.img`)
never touches the cache, so it can alternate with Android.

## Updates and apps

The installed system updates itself as the Redmi 2's does
([updates](redmi2.md#updates), [apps](redmi2.md#apps)): **UPDATES → CHECK
FOR UPDATES** reads
`https://freshpsam.github.io/sos2/gta7lite/sos2-gta7lite.update` and the
AArch64 apps' `apps-arm64/index.txt`; **INSTALL SYSTEM** downloads the boot
image, about 29 MB, which the kernel checks and writes to the boot partition
as `fastboot flash boot` does (below), and **RESTART** starts it. Apps
install, update and are removed one by one in the app store. The update file
holds the boot image after its 4 KiB header (include/system_store.h), padded
with zeros to whole 2 KiB pages, since Samsung's trailer ends the image 16
bytes into one; the kernel finds the image's end from its own header. AArch64
apps carry EM_AARCH64 in their ABI (include/user_abi.h), so neither device
takes the other's. A RAM session installs nothing: there **UPDATES** restarts
into download mode instead.

`make publish` publishes each device built, keeping the other device's
published files; `make publish PUBLISH=gta7lite` publishes the tablet alone.
Tried on the tablet: an update from one published release to the next, the
Calculator's install, and the restart, after which the saved network joined
again by itself.

## Flashing

Flashing needs the bootloader unlocked, which wipes the tablet and trips Knox
permanently, and the stock vbmeta with verification disabled. In download
mode, with Ubuntu's `heimdall-flash` and ModemManager stopped, from
`build/gta7lite`. SOS2 restarts into download mode from **UPDATES → DOWNLOAD
MODE**, or from the computer with `fastboot reboot bootloader` (USB link
below): it sets the reboot reason Samsung's kernel leaves for "reboot
download" in MediaTek's boot-params buffer, which LK names in
`/chosen/ram_console`, after completing the buffer's header as Android's
kernel does at each boot (boards/gta7lite/board.c). Otherwise unplug the cable,
hold Volume Up and Volume Down, plug it in, then press Volume Up at the
warning.

```sh
heimdall flash --boot boot.img        # RAM session
heimdall flash --boot flash-boot.img  # installed system
```

Both have been tried.

## USB link

While SOS2 runs, its USB port is a fastboot device for the computer it is
plugged into (boards/gta7lite/usb.c and fastboot.c). It has fastboot's
generic IDs, 18d1:d00d, which Android's udev rules give the desktop user, so
the fastboot tool needs no root. The host sees it once the desktop runs; a
port that failed to start says why in the desktop's status lines.

```sh
fastboot getvar version-os      # the running release
fastboot flash boot flash-boot.img
fastboot reboot
fastboot reboot bootloader      # Samsung's download mode
```

`fastboot flash boot` takes only the installed system's image
(`build/gta7lite/flash-boot.img`), checked as tools/package_gta7lite.py
builds it, and writes it as the updater writes an update: the first sector
cleared, the rest written and read back, the first sector last. A new image
needs no hands on the tablet:

```sh
fastboot flash boot flash-boot.img && fastboot reboot
fastboot getvar version-os      # waits for the new system
```

A panic keeps the link up, so the tablet can be restarted and, when no
storage job was cut short, a fixed image flashed. When SOS2 does not start,
Heimdall flashes from download mode.

All of these have been tried on the tablet: a flash takes about 41 s (the
28 MB image sent in 4 s, written and read back), the tablet answers again
30 s after `fastboot reboot`, and download mode comes up 7 s after `fastboot
reboot bootloader`; `heimdall download-pit --output pit.bin` leaves it
without writing anything.

## Loader and hardware

- **Loader:** Samsung's LK starts the boot partition's image: the kernel at
  0x40080000, which must end below Samsung's debug logs at 0x46C00000, the
  device tree at 0x47880000 and the ramdisk at 0x51B00000. Its command line
  must keep `bootopt=64S3,32N2,64N2`, LK's switch for a 64-bit kernel.
- **Display:** LK leaves its framebuffer scanning out and names it in the
  device tree (`/chosen/atag,videolfb`: base, size, the panel's driver). The
  kernel draws on it as 800×1340 ARGB8888 with opaque alpha, scaling the
  480×800 desktop.
- **CPUs and interrupts:** eight Cortex-A53 cores in two clusters, started
  through PSCI; the architected timer's interrupt through the GICv3 at
  0x0C000000 (redistributors from 0x0C100000). Restart is PSCI's system
  reset. In standby the other seven cores power down, as MediaTek's kernel
  takes cores offline (boards/gta7lite/cpu_power.c): each leaves through
  PSCI's CPU_OFF, then CPU 0 cuts its power through the ATF's SiP call, and
  the second cluster's once its four cores are down. The ATF's CPU_OFF alone
  leaves a core powered, and CPU_ON could then never restart it. At the wake
  CPU_ON restarts them. The kernel runs under GenieZone, MediaTek's
  hypervisor, which passes these calls to the ATF.
- **Watchdog:** LK leaves TOPRGU's watchdog (0x10007000) running; the kernel
  feeds it from boot, while the desktop runs and on its error screen.
- **Touch:** the touch half of the panel's Novatek NT36523, on SPI1
  (0x11010000, pins GPIO 25-28 as the stock boot sets them). The kernel
  drives it as the tablet's Linux does (Samsung's gta7lite kernel): 10 us of
  chip-select setup, hold and idle at 1 MHz, the firmware's hardware-CRC
  download, then one 108-byte report per rising edge of the controller's
  interrupt line (GPIO 10). The edge latches in MT6765's external interrupt
  controller (0x1000B000, EINT 10), which the main loop polls; its GIC line
  stays off. The first finger down is the pointer.
- **Home and Back:** the tablet has neither key, so the top bar shows CLOSE,
  HOME and BACK side by side over a page, and HOME and BACK on the Wi-Fi
  screen. They act as the phone's keys do; BACK is on the right, as on
  Samsung's navigation bar.
- **Randomness:** as on the phone, jitterentropy's CPU timing jitter,
  measured with the generic timer, with its startup and continuous health
  tests (kernel/protection/jitter_entropy.c), seeds an HMAC_DRBG with SHA-256
  (NIST SP 800-90A) that answers every request: 48 bytes of jitter at the
  first request and every 10 minutes or 1 MiB after. A jitter collection takes
  about 55 ms per 32 bytes, which pages used to wait for on every TLS
  connection. Its startup tests feed the watchdog.
- **CPU clocks:** LK leaves cluster 0 (CPUs 0-3) at 1500 MHz, cluster 1
  (CPUs 4-7) at 920 MHz and their interconnect at 600 MHz, on VPROC 0.8 V.
  boards/gta7lite/cpu_clock.c raises them to MediaTek's top operating point
  for this chip's 2.3 GHz bin (2301, 1800 and 1051 MHz on 1.11875 V, VSRAM_PROC
  1.125 V), as the stock kernel's cpufreq_v1 sequences it: the supplies first
  going up, the clocks first going down. The main loop raises them once pages
  use a tenth of a CPU over 20 ms and lowers them after a second below that
  and in standby: at the top point the tablet draws about 66 mA more even
  idle (316 against 250 mA, screen on). There is no thermal throttling.
- **PMIC:** the MT6357, through MT6765's PMIC wrapper (0x1000D000), which LK
  leaves running (boards/gta7lite/pmic.c). Its RTC, which Android keeps in
  UTC, is the clock. Its power key turns the screen off and on: standby
  turns the backlight off, puts the panel to sleep and keeps the desktop and
  pages frozen, as on the phone. The battery icon's level is estimated from
  the battery's voltage through the stock open-circuit voltage table (25 C),
  steadied as on the phone, and shows a charger and whether the MT6370
  charges.
- **Backlight and charger:** the MT6370 at address 0x34 on I2C5
  (0x11016000), driven in FIFO mode with LK's timing
  (boards/gta7lite/mt6370.c and i2c.c). Its backlight driver lights the panel at the
  brightness the panel's own PWM output sets, and standby clears its enable;
  its charger charges the battery, as LK set it up: the kernel does not
  supervise charging.
- **Standby:** the panel sleeps with display off (28h) and sleep in (10h),
  sent inside video mode as MediaTek's driver sends DCS commands to video
  panels, and the overlay engines stop reading the framebuffer; its reset
  line and supplies stay on, so sleep out (11h) and display on (29h) wake it
  without its init table (boards/gta7lite/display.c). Measured with the
  battery supplying everything (the MT6370's input in high impedance, the
  USB link up), standby draws 117.5 mA, from 244 mA (awake: 245 mA): the
  backlight's 64 mA, the panel's 36 mA, the overlay reads' 15 mA and the
  seven cores' 11 mA. Stopping the DSI link would save about 6 mA more.
- **Storage:** the eMMC on MSDC0 (0x11230000), in PIO, adopting LK's clock,
  bus and timing (boards/gta7lite/emmc.c); see the installed system above.
  The card answers at RCA 1. Its primary GPT has a 512-byte header and 56
  entries; the backup lists 128 entries without a disk GUID, so SOS2 reads
  the primary alone, as Linux does (kernel/protection/disk_partition.c).
- **USB:** MT6765's MUSB controller (0x11200000) and its PHY (0x11CC0800),
  brought up as MediaTek's driver does: VUSB33 from the PMIC, infracfg's
  ICUSB clock and topckgen's usb_top_sel, the PHY's power-on sequence with
  device mode and VBUS forced, then the controller's soft reset. The main
  loop polls it: programmed I/O, high speed, one bulk endpoint each way.
- **Wi-Fi:** the MT6765's connectivity subsystem with its MT6631 radio chip,
  started as MediaTek's drivers start it (boards/gta7lite/connsys.c): its
  supplies from the PMIC, the radio chip's pins, its power domain, and its
  4.25 MiB of memory at 0xBF000000 holding its RAM code; then, over BTIF
  (0x1100C000) in MediaTek's STP framing, its ROM's patch, a WMT reset, STP's
  full mode, the chip's ID and RF calibration. BTIF takes 16 bytes at a time,
  100 us apart, and each command waits 20 ms after the ROM's last answer:
  sent faster, the ROM loses bytes or stays out of its patch mode. Wi-Fi
  itself then runs on DMA rings (0x18004300, with a 3 MiB pool at
  0xBE000000) as MediaTek's gen4m driver runs it (boards/gta7lite/
  wifi_radio.c): its firmware download, 2.4 GHz scans of channels 1-11,
  joins, the hardware's CCMP, and frames converted between the hardware's
  Ethernet form and the 802.11 frames of the station and network device
  the phone shares (kernel/wifi). Both memory regions stay out of every allocation. Tried: one
  WPA2-PSK router, its key exchange, DHCP and pages; open and WPA3 networks
  are not. The subsystem starts once per boot, so a failure stays until the
  next restart. In standby the station leaves its network and the chip goes
  back to its firmware, which sleeps it (gen4m's FW own); waking takes it
  back, and the station rejoins.
- **Sound:** the two speakers, played as the stock kernel plays music
  through them (boards/gta7lite/audio.c). MT6765's audio front end
  (0x11220000) plays a 144 ms ring in its own SRAM through its DL1 memory
  interface and downlink, which sends the samples over the MTKAIF pins
  (GPIO 136-139) to the MT6357's DAC; the MT6357's headphone drivers feed two
  Awinic AW87359 amplifiers at 0x58 and 0x59 on I2C3, set to the stock
  `aw87xxx_acf.bin`'s Music profile (the stock speaker path, `int_hp_buf`).
  I2C3's registers for the AP are a channel 0x100 in, which keeps the bus
  after a failed transfer until its arbitration is resumed. Headphones in
  the jack turn the amplifiers off: the MT6357's ACCDET senses the plug
  through its EINT0 comparator, polled every 100 ms. Both outputs run at
  Android's analog level (the headphone drivers at +2 dB), and the volume
  scales the samples: 15 steps of 3 dB and silence, kept apart for the
  speakers (12, -9 dB, at each start) and the headphones (7, -24 dB). The
  clocks, codec and amplifiers power up when a stream has frames, in about
  30 ms, and down 3 s after the last; the main loop's polls keep the ring
  filled up to 133 ms ahead of the DMA, with silence behind the audio, so a
  volume change is heard within that. Tried by hand: the speakers play and
  the volume keys step them; also the DMA at 48 kHz, both amplifiers on and
  off, the clocks gated while silent, and the jack read as empty. Headphones
  are not yet tried.
- **Volume keys:** Volume Up is the keypad's KPCOL0 (GPIO 93; key 1 of the
  keypad controller at 0x10010000, which LK leaves scanning), Volume Down
  the MT6357's home key (boards/gta7lite/keys.c). The main loop samples
  them every 20 ms; a press steps the volume of the output in use, a held
  key repeats after half a second, and the header shows the level for 2 s
  in place of the clock, as a bar: white for the speakers, green for
  headphones, which it also shows when a plug goes in or out.
- **No vibrator:** the stock vendor partition has no vibrator HAL, and
  pulses of the PMIC's VIBR supply, which MediaTek's reference device tree
  names, are not felt.
- **Not yet:** internet time over Wi-Fi, untried; whole-chip
  sleep through the SPM, MediaTek's power manager, on which the phone's
  standby rests. LK loads the SPM's firmware for the tablet's LPDDR3-1866 and
  starts the SSPM, which answers the kernel's suspend messages; but PSCI's
  CPU_SUSPEND into the SPM's suspend state (0x01010005), through GenieZone,
  never returned to the kernel, and the watchdog restarted the tablet. The
  ATF logs to RAM at 0xBFF30000, LK's atf-log-reserved, but clears the log
  when it starts.
