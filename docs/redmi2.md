# Redmi 2

Target: Xiaomi Redmi 2 **WT88047 / MSM8916** (ARM32) with the
[lk2nd-msm8916](https://github.com/msm8916-mainline/lk2nd) loader. Packaging
only writes files on the computer; it never touches a phone. SOS2 starts from
the computer with `fastboot boot`, or [installed](#installing) on the phone,
where it [updates itself](#updates). QEMU does not model the phone's
hardware, so only the handset qualifies it.

## RAM-only session

```sh
make fetch && make engine # once
make phone                # build/redmi2-browser-ram/boot.img and boot.img.json
```

Copy `boot.img` to the computer connected to the phone and check its SHA-256
against `sha256.boot_image` in `boot.img.json` (`certutil -hashfile boot.img
SHA256` on Windows): lk2nd starts whatever it receives. Start **lk2nd
fastboot** (hold Volume Down while the phone starts) and run:

```sh
fastboot boot boot.img    # ./fastboot.exe in Windows Git Bash
```

This runs SOS2 from RAM without touching the eMMC; credentials and website
data last until reboot.

1. Wait on the loading screen (Wi-Fi start-up gives up after 90 s).
2. Tap **WI-FI**, **Scan Networks**, pick a network, enter its password and
   tap **Connect**; the header shows the time once the network has it.
3. Enter an address such as `https://example.com/` and tap **Open**. The
   right-hand key below the screen is Back.

If Wi-Fi fails the desktop says why ([Wi-Fi](#wi-fi)). Randomness
comes from CPU jitter, which seeds a generator (as on the
[tablet](gta7lite.md)); if its self-test fails, pages, Wi-Fi and HTTPS stay
unavailable. Power toggles standby, which lasts about two weeks. Pages play
[sound](#sound) through the loudspeaker; the volume keys set its level.

## Wi-Fi

The native WCN3620 (Pronto v2) [driver](#loader-and-hardware) passes Ethernet
frames to lwIP. Firmware startup, scanning, association, DHCP and browsing
work on the handset; router interoperability and long-running recovery still
need qualification (see the [roadmap](roadmap.md)).

**WI-FI → Scan Networks** lists up to 128 networks across pages; pick one,
enter its password and tap **Connect**. The saved network has its own row at
the top, which the list leaves out. If more networks are discovered,
the list says **Some networks could not be listed**. The keyboard is ASCII,
and without a saved profile the radio stays idle until you scan.

- Supported: open, WPA2-Personal (CCMP) and WPA3-SAE, with no silent
  downgrade, on 2.4 GHz channels 1–11.
- Not supported: Enterprise, OWE, TKIP, hidden networks and roaming.
- A profile is saved only after it connects; a wrong password keeps the
  previous one. The saved network's page shows its password (**Show**) and
  offers **Forget**.
- A RAM session keeps the profile until reboot; persistent storage, an
  [installed](#installing) phone's included, keeps it in `/settings/wifi`,
  unencrypted, outside every profile.

Failures show their reason on the Wi-Fi screen; transport, DMA or entropy
failures need a reboot.

### Power save

Standby leaves the network and puts the firmware in idle-mode power save, as
Linux's `wcn36xx` does; waking rejoins within a few seconds.

### Provisioning

An optional bootstrap profile can be built into the image. The tool prompts
for the password without echo and never overwrites a file:

```sh
python3 tools/redmi2_wifi.py --ssid 'browser lab' --security wpa3-sae \
  --output "$HOME/sos2-wifi.json"
make phone REDMI2_WIFI_PROFILE="$HOME/sos2-wifi.json"
```

The profile is imported once it connects and only when no desktop record
exists, so **Forget** survives reboot. Keep the profile and any image
containing it private.

## Installing

`make phone` also writes the installed system to `build/redmi2-install`, and
`make publish` puts it on [GitHub Pages](https://freshpsam.github.io/sos2/),
with the [tablet's](gta7lite.md#updates-and-apps) when that is built too
(`PUBLISH=redmi2` publishes the phone alone):

- `flash-boot.img`: the whole system, as the RAM image holds it (kernel, DTB
  and the compressed resources as its ramdisk), with `sos2.storage=installed`
  in its command line.
- `sos2-redmi2.update`: the same image, for the [updater](#updates).
- `install.json`: the release, the files' sizes and SHA-256s, and these
  commands.

Check the files' SHA-256 against `install.json`, start lk2nd fastboot and run:

```sh
fastboot flash boot flash-boot.img     # lk2nd writes it after its own 512 KiB
fastboot reboot
```

This replaces the system lk2nd starts from the boot partition (such as
MIUI's kernel); lk2nd and every other partition stay. MIUI comes back by
flashing its own `boot.img`. At power-on lk2nd loads the whole image, as
`fastboot boot` does, and the kernel keeps web pages', apps' and the
desktop's data in the cache partition, which Android treats as disposable.

The 320 MiB cache partition holds 1 MiB left unused, the [app store](#apps)
and the 259 MiB [data volume](#persistent-storage)
(`include/system_store.h`), which SOS2 sets up at its first start, whatever
the partition held. SOS2 finds the `boot` and `cache` partitions by name in a
GPT it validates, and writes only inside them; when the eMMC fails, the phone
runs as a RAM session.

## Updates

The updater (**UPDATES** on the desktop) shows the running release (UTC build
time and commit, `+` for uncommitted changes) and checks
`https://freshpsam.github.io/sos2/redmi2/sos2-redmi2.update` over HTTPS; it
needs Wi-Fi and the network time.

1. **CHECK FOR UPDATES** reads the published release and app list.
2. **INSTALL SYSTEM** (**REINSTALL** for the running release) downloads the
   boot image, about 24 MB, into RAM. The kernel checks it, clears the boot
   image's first sector, writes the rest and reads it back, and writes the
   first sector last.
3. **RESTART** restarts the phone into the new release.

A failed or interrupted download leaves the running release to start again.
Once the write has begun, closing the updater does not stop it, but a power
loss before it ends (likely about a minute of single-sector writes; not yet
timed on the handset) leaves lk2nd in fastboot, where flashing
`flash-boot.img` finishes the job. Updates are not signed: their SHA-256s guard against damage, and
whoever can publish to the Pages site can install software on the phone.
Pages is public, so `make publish` refuses a build with a Wi-Fi profile; and
since the updater downloads over Wi-Fi, it refuses one without the Wi-Fi
firmware, which would leave a device that installed it unable to update. A
RAM session or QEMU shows updates but installs nothing.

## Apps

The phone's images build in no apps. Native apps such as the calculator are
packages, `<id>.sos2app` (`include/app_store.h`); `make phone` writes them to
`build/apps` with an `index.txt`, and `make publish` puts them under
`https://freshpsam.github.io/sos2/apps/`. After **CHECK FOR UPDATES** the
updater lists every published or installed app:

- **INSTALL** or **UPDATE** downloads a package; the kernel checks it and its
  program and writes it into the app store. It starts at its next launch.
- **REMOVE** takes it out and deletes its files. A running app is neither
  updated nor removed.

An app built for another system ABI waits for a matching system or app
update. The app store holds ten apps, each written to the half of its
position not in use, so an interrupted install keeps the previous version.

## Packaging options

- `REDMI2_WIFI_FIRMWARE=folder` supplies `wcnss.mdt`, its `wcnss.bXX`
  segments and the board calibration `WCNSS_qcom_wlan_nv.bin`; the default,
  `third_party/redmi2-firmware`, holds this project's handset files. Another
  handset must use its own files, never another phone's NV calibration. The
  packager checks the signed MDT's layout; the secure monitor verifies its
  signatures at boot.
- `REDMI2_WIFI_PROFILE=file` adds a bootstrap Wi-Fi profile
  ([provisioning](#provisioning)).
- The [Adreno path](gpu.md#redmi-2-adreno) is on once `make fetch` has put
  its firmware in `build/redmi2-adreno`; `REDMI2_ADRENO_FIRMWARE` names
  another folder, or none (empty) for CPU composition.
- `REDMI2_JSC=options` passes JavaScriptCore options to every page through
  the `sos2.jsc=` boot argument, such as `useJIT=false` (interpreter only) or
  `useDFGJIT=false` (up to the baseline JIT), to compare the tiers. The
  installed image and update package carry the same options as the RAM image.
  The kernel keeps 255 bytes of `sos2.*` arguments, which leaves the options
  their whole 127 characters.
- `STORAGE=persistent` builds `build/redmi2-browser/boot.img`, which keeps
  data on a [dedicated partition](#persistent-storage).

## Persistent storage

The persistent image (started with `fastboot boot`) keeps profiles on one
dedicated GPT partition through the native eMMC driver. No SOS2 tool creates
phone partitions. The kernel accepts only a single partition named
`sos2-data` with type GUID `0f8d5c18-7f04-4af2-98d5-937c32e075a7`, 4 KiB
aligned, 2 MiB to 1 GiB, in a valid primary GPT, holding a littlefs volume
written by `build/host/mkfs`; otherwise storage is unavailable for the boot.
An [installed](#installing) phone instead keeps its volume in the cache
partition after the app store and never looks for `sos2-data`. The kernel
formats that volume when it does not mount, as at the first start, and runs
as a RAM session when the eMMC fails. Power loss and durability are not
qualified.

The driver (boards/redmi2/emmc.c, on the SD host code it shares with the
Nexus 5, boards/qcom/sdhci.c) adopts the eMMC controller as lk2nd leaves it
and transfers single 512-byte sectors by PIO, without DMA, erase or
partition switches; any failure stops storage for the boot, and writes are
never retried.

## Loader and hardware

- **Loader:** lk2nd 23.1 (`third_party/lk2nd`). The packager builds an
  Android boot v0 image from the kernel, the WT88047 DTB
  (`boards/redmi2/boot.dts`) and an LZMA-compressed resource ramdisk, kept
  below the 90 MiB lk2nd maps for `fastboot boot`. Images ask lk2nd to move
  its framebuffer (`lk2nd.pass-simplefb=autorefresh,relocate`).
- **Keys:** Menu, Home and Back report through the touch controller below the
  display rows; Home shows the desktop and Back goes back, never to the
  desktop. Menu is unused. Volume Up (GPIO 107) and Volume Down (the
  PM8916's RESIN) step the [loudspeaker's](#sound) volume, which the header
  shows in place of the clock for 2 s.
- **Devices:** FT5x06 touch; PM8916 power key, backlight, vibrator, RTC,
  restart and voltage ADC; SMB1360 charger and gauge; CPU-jitter entropy;
  WCN3620 [Wi-Fi](#wi-fi); the eMMC; an optional Adreno 306; the
  [loudspeaker](#sound). All four cores start; a core that does not is
  skipped.
- **Wi-Fi:** the stock
  [WT88047 kernel](https://github.com/LineageOS/android_kernel_wingtech_msm8916/tree/beb5692ed25808aebce7b57e318cf8bd33e645d8)
  (its WCNSS, prima/DXE and SCM sources) defines the board contract, and
  Linux v6.12's
  [wcn36xx driver](https://github.com/torvalds/linux/tree/v6.12/drivers/net/wireless/ath/wcn36xx)
  adds wire definitions; where they differ, the stock driver wins. The driver
  loads the firmware through the secure monitor, talks to it over SMEM, SMP2P
  and SMD, and drops its start-up power requests once the firmware is ready.
  Radio work is polled on the kernel service worker, with no interrupts.
  Authentication uses vendored hostap 2.12 and Mbed TLS 3.6.7 with no Linux
  daemon. When the firmware rejects a calibration upload fragment after
  accepting the first (`status=00000005`), SOS2 restarts the upload once.
- **Restart:** a PMIC hard reset through PS_HOLD, as lk2nd's `reboot_device`
  does.
- **Sound:** see [Sound](#sound).
- **Battery:** the header's icon shows a level estimated from the battery's
  voltage, as on the tablet, and the SMB1360's USB input, which alone marks
  the battery charging, read every 30 s with the screen on and never in
  standby (`boards/redmi2/battery.c`). The voltage is VPH_PWR, which the
  battery feeds, through the PM8916's voltage ADC (VADC 0x3100, channel 7
  behind a 1:3 divider, calibrated against its 0.625 V and 1.25 V
  references as Linux's `qcom-spmi-vadc.c` does). The stock DT turns off
  the PM8916's own charger and BMS (`use-external-charger`, `disable-bms`).
  The level comes from the 25 C open-circuit voltages of Qualcomm's QRD SKUI
  profile for a 4.35 V, 2200 mAh cell like the BM44
  (`batterydata-qrd-skui-4v35-2200mah.dtsi`), with 0% at its 3.4 V cutoff.
  A reading carries the load of its moment, so the level takes the highest
  reading of the last 2.5 minutes off a charger, with 45 mV added back for
  an awake phone's lightest load, and the lowest on one; only a load held
  that long reads low. The SMB1360 gauge's own level is used only
  when the VADC cannot be read: its readings refresh rarely at this phone's
  currents, and it stayed at 100% for hours. SOS2 only reads the SMB1360: it
  sets no charger limit and never resets the gauge, whose reset estimated
  the charge afresh from the voltage of the moment and made the level jump
  between half and near empty. The level is steadied: off a
  charger it only falls, on one it only rises, and a step of 5% or more waits
  for three consecutive readings within two percentage points, spanning at
  least a minute. Smaller steps in that direction update immediately, even
  during brief wake-ups. After boot, standby or three failed reads, that
  confirmation allows either direction, since charging may have changed
  while unobserved.

## Sound

Pages and apps play through the loudspeaker (`boards/redmi2/audio.c`), as
mainline Linux drives MSM8916 sound without the modem's DSP (`lpass-cpu.c`,
`lpass-platform.c`, `lpass-apq8016.c`, `apq8016_sbc.c`, `msm8916-wcd-*.c`,
`gcc-msm8916.c`, `msm8916-wingtech-wt88047.dts`), with the analog power-up
and power-down sequences of the stock kernel (`msm8x16-wcd.c`, whose audio
interface the DSP ran) and the speaker route of LineageOS's mixer paths:

- LPAIF's first read DMA channel plays a 256 ms ring of 16-bit stereo frames
  in uncached kernel memory over the primary MI2S port (1.536 MHz bit clock
  from the crystal) to the SoC's digital codec (9.6 MHz master clock), whose
  RX1 and RX2 paths cross the PDM link (GPIO 63-68) to the PM8916's analog
  codec; its headphone amplifiers feed the loudspeaker's amplifier, from the
  right one, which GPIO 117 enables.
- As the stock speaker path sends one channel, the loudspeaker gets the mix
  in mono. The digital gains stay at Android's 0 dB; the volume scales the
  samples, 15 steps of 3 dB and silence, 12 (-9 dB) at each start, as on the
  tablet.
- The clocks, codecs and amplifier power up when a stream has frames, in
  about 25 ms, the amplifier last, and down 3 s after the last frame; until
  then standby keeps its 20 ms polls rather than sleeping the whole chip.
  The main loop's polls keep the ring filled up to 245 ms ahead of the DMA,
  with silence behind the audio, so a volume change is heard within that. A
  DMA that stops for half a second (judged by polls less than a lap apart),
  or a power-up that fails, leaves pages playing silently until the next
  boot.
- Tried by hand: the loudspeaker plays and the volume keys step it. Not
  yet: headphones. Nothing senses the jack (the PM8916's MBHC), and its
  switch (GPIO 8) stays open, so the loudspeaker plays with a plug in. The
  microphones are unused.
