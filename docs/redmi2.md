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

If Wi-Fi fails the desktop says why ([details](redmi2-wifi.md)). Randomness
comes from CPU jitter, which seeds a generator (as on the
[tablet](gta7lite.md)); if its self-test fails, pages, Wi-Fi and HTTPS stay
unavailable. Power toggles standby, which lasts about two weeks.

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
   boot image, about 29 MB, into RAM. The kernel checks it, clears the boot
   image's first sector, writes the rest and reads it back, and writes the
   first sector last.
3. **RESTART** restarts the phone into the new release.

A failed or interrupted download leaves the running release to start again.
Once the write has begun, closing the updater does not stop it, but a power
loss before it ends (likely about a minute of single-sector writes; not yet
timed on the handset) leaves lk2nd in fastboot, where flashing
`flash-boot.img` finishes the job. Updates are not signed: their SHA-256s guard against damage, and
whoever can publish to the Pages site can install software on the phone.
Pages is public, so `make publish` refuses a build with a Wi-Fi profile. A
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
  segments and `WCNSS_qcom_wlan_nv.bin`; the default,
  `third_party/redmi2-firmware`, holds this project's handset files. Another
  handset must use its own files, never another phone's NV calibration.
- `REDMI2_WIFI_PROFILE=file` adds a bootstrap Wi-Fi profile
  ([provisioning](redmi2-wifi.md#provisioning)).
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

The driver adopts the eMMC controller as lk2nd leaves it and transfers single
512-byte sectors by PIO, without DMA, erase or partition switches; any
failure stops storage for the boot, and writes are never retried.

## Loader and hardware

- **Loader:** lk2nd 23.1 (`third_party/lk2nd`). The packager builds an
  Android boot v0 image from the kernel, the WT88047 DTB
  (`boards/redmi2/boot.dts`) and an LZMA-compressed resource ramdisk, kept
  below the 90 MiB lk2nd maps for `fastboot boot`. Images ask lk2nd to move
  its framebuffer (`lk2nd.pass-simplefb=autorefresh,relocate`).
- **Keys:** Menu, Home and Back report through the touch controller below the
  display rows; Home shows the desktop and Back goes back, never to the
  desktop. Menu is unused.
- **Devices:** FT5x06 touch; PM8916 power key, backlight, vibrator, RTC and
  restart; SMB1360 battery gauge; CPU-jitter entropy; WCN3620
  [Wi-Fi](redmi2-wifi.md); the eMMC; an optional Adreno 306. All four cores
  start; a core that does not is skipped.
- **Restart:** a PMIC hard reset through PS_HOLD, as lk2nd's `reboot_device`
  does.
- **Battery:** the header's icon shows the SMB1360 gauge's level and its
  charger's USB input, which alone marks the battery charging, read every
  30 s with the screen on and never in standby. SOS2 only reads the gauge: it
  sets no charger limit and never resets it. The level is steadied: off a
  charger it only falls, on one it only rises, and a step of 5% or more waits
  for three consecutive readings within two percentage points, spanning at
  least a minute. Smaller steps in that direction update immediately, even
  during brief wake-ups. After standby or three failed reads, that
  confirmation allows either direction, since charging may have changed
  while unobserved.
  Below the current Linux's setup counts as standby (200 mA) the gauge
  refreshes its readings rarely, so the level can lag.
