# SOS2 (𝚂mall Oper𝚊ting Syste𝚖 v2)

SOS2 is a web-first operating system: apps are web pages, plus native apps
such as a calculator. Native WebKit (WPE WebKit 2.52.6, built as the `SOS2`
port) runs as one process per page on SOS2's own kernel, with no Linux, and
JavaScriptCore's JIT compiles page scripts. The ARM32 system runs in QEMU and
on the Xiaomi Redmi 2 (MSM8916, WT88047); the AArch64 system runs in QEMU and
on the [Galaxy Tab A7 Lite](docs/gta7lite.md) (SM-T220).

The desktop is the browser shell: **WI-FI** and **UPDATES** buttons, an
address field with **Open** above the open pages, an **Apps** launcher and a
touch keyboard. Each page or app is an isolated process. A page's header has
**Close**, the UTC time and the battery; the phone's Home key (Alt+Home in
QEMU) shows the desktop, its Back key (Alt+Left in QEMU) goes back but never
to the desktop, and the power button toggles standby.

Pages run HTML, CSS, JavaScript, images, CJK text and emoji over verified
HTTPS, with cookies and web storage, drawn by Skia on the CPU (or the GPU with
[virgl](docs/gpu.md) in QEMU). The Redmi 2 browses over Wi-Fi with its data in
RAM, lasts about two weeks in standby, and can be
[installed](docs/redmi2.md#installing) on its eMMC, where the updater installs
new releases and [apps](docs/redmi2.md#apps) from
[GitHub Pages](https://freshpsam.github.io/sos2/).

## Build and run

Needs Linux with Python 3.12+, GNU Make, `arm-none-eabi-` GCC/G++ with C++23
(GCC 14.2), `qemu-system-arm` 10.2+, `dtc` and host C and C++ compilers; the engine
also needs CMake, Ninja, Meson, Ruby, Perl with JSON::PP, gperf, Bison, flex,
Python's Mako, pkg-config and patch. `ARCH=arm64` builds and runs the AArch64
system instead, with `aarch64-none-elf-` GCC/G++ 14.2 (the Arm GNU Toolchain,
on `PATH` or unpacked in `.tools/aarch64-none-elf`) and `qemu-system-aarch64`.

```sh
make          # QEMU kernel and filesystem tools
make fetch    # download pinned engine dependencies and Adreno firmware
make engine   # build the runtime, WebKit, the updater, the apps and the bundle
make run      # boot the desktop in QEMU with a temporary RAM session
make phone    # build the phone's boot.img, installed system and app packages
make publish  # push the installed system and apps to the gh-pages branch
make tablet   # the Galaxy Tab A7 Lite's kernel, ramdisk and boot.img (arm64)
```

Only `make fetch` downloads, and nothing touches a phone or tablet. `make help` lists
the options, such as `STORAGE=persistent` (a disk image in QEMU),
`ENGINE=path/to/native-web.bundle`, `JOBS=8` and `RUN_ARGS='--cpus 1'` or
`'--graphics virgl'`; `RUN_ARGS='--jsc useJIT=false'` runs pages on the
interpreter and `'--jsc useDFGJIT=false'` stops at the baseline JIT. `make clean` deletes `build/`, including downloads and the persistent
QEMU disk.

## Source map

- `arch/arm`, `arch/arm64`, `kernel`, `include`: the kernel (ARM32 and
  AArch64) and the desktop.
- `boards/redmi2`: the Redmi 2's drivers, their headers and its linker script.
- `boards/gta7lite`: the Galaxy Tab A7 Lite's drivers: bootloader handoff,
  watchdog, PMIC, touch, eMMC, USB and Wi-Fi.
- Each board's directory is on the include path only for that board's build;
  shared code includes its headers inside `#ifdef SOS2_BOARD_*`.
- `sdk`: the C/C++ runtime and syscall interface for programs.
- `programs/browser`: the engine, `/bin/sos2-web`.
- `programs/updater`: the updater, `/bin/sos2-updater`.
- `apps`: native apps, packaged and published apart from the system.
- `tools`: build, launch, packaging and publishing scripts.
- `third_party`: vendored libraries, trust data and the Redmi 2's Wi-Fi
  firmware, with their licenses.

## Documentation

- [Architecture](docs/architecture.md): processes, storage, networking,
  rendering, apps and updates.
- [GPU acceleration](docs/gpu.md): virgl in QEMU and the Redmi 2's Adreno.
- [Redmi 2](docs/redmi2.md): booting, installing, updating and the hardware.
- [Redmi 2 Wi-Fi](docs/redmi2-wifi.md): networks, profiles and the driver.
- [Galaxy Tab A7 Lite](docs/gta7lite.md): the AArch64 port's boot image and
  the tablet's hardware.
- [Roadmap](docs/roadmap.md): open work.
