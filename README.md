# SOS2 (𝚂mall Oper𝚊ting Syste𝚖 v2)

SOS2 is a web-first operating system: apps are web pages, plus native apps
such as a calculator. Native WebKit (WPE WebKit 2.52.6, built as the `SOS2`
port) runs as one process per page on SOS2's own kernel, with no Linux, and
JavaScriptCore's JIT compiles page scripts. The ARM32 system runs in QEMU, on
the Xiaomi Redmi 2 (MSM8916, WT88047) and on the [Nexus 5](docs/nexus5.md)
(hammerhead); the AArch64 system runs in QEMU and on the
[Galaxy Tab A7 Lite](docs/gta7lite.md) (SM-T220), and the
[x64](docs/x64.md) system in QEMU's microvm and, from a USB stick, on
QEMU's emulated PC.

The desktop is the browser shell: **WI-FI** and **UPDATES** buttons, an
address field with **Open** above the open pages, an **Apps** launcher and a
touch keyboard. Each page or app is an isolated process. A page's header has
**Close**, the UTC time and the battery; the phone's Home key (Alt+Home in
QEMU) shows the desktop, its Back key (Alt+Left in QEMU) goes back but never
to the desktop, and the power button toggles standby.

Pages run HTML, CSS, JavaScript, images, text in Latin script and emoji over
verified HTTPS, with cookies and web storage, drawn by Skia on the CPU (or the GPU with
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
on `PATH` or unpacked in `.tools/aarch64-none-elf`) and `qemu-system-aarch64`;
`ARCH=x64` the x64 system, with the `x86_64-elf-` GCC/G++ 14.2 that
`make ARCH=x64 toolchain` builds into `.tools/x86_64-elf` (from the sources
`make ARCH=x64 fetch` downloads),
`qemu-system-x86_64`, NASM and, for KVM, access to `/dev/kvm`; `make pc` and
`make pc-run` also need OVMF.

```sh
make          # QEMU kernel and filesystem tools
make fetch    # download pinned engine dependencies and Adreno firmware
make engine   # build the runtime, WebKit, the updater, the apps and the bundle
make run      # boot the desktop in QEMU with a temporary RAM session
make phone    # build the phone's boot.img, installed system and app packages
make publish  # push the installed system and apps to the gh-pages branch
make tablet   # the Galaxy Tab A7 Lite's kernel, ramdisk and boot.img (arm64)
make nexus5   # the Nexus 5's boot.img, installed system and apps (arm32)
make pc       # a PC's USB stick image, installed system and apps (x64)
make pc-run   # that stick on QEMU's emulated PC (q35 with OVMF)
```

Only `make fetch` downloads, and nothing touches a phone or tablet. `make help` lists
the options, such as `STORAGE=persistent` (a disk image in QEMU),
`ENGINE=path/to/native-web.bundle`, `JOBS=8` and `RUN_ARGS='--cpus 1'` or
`'--graphics virgl'`; `RUN_ARGS='--jsc useJIT=false'` runs pages on the
interpreter and `'--jsc useDFGJIT=false'` stops at the baseline JIT. `make clean` deletes `build/`, including downloads and the persistent
QEMU disk.

## Source map

- `arch/arm`, `arch/arm64`, `arch/x64`, `kernel`, `include`: the kernel
  (ARM32, AArch64 and x64) and the desktop. `kernel/main.c` starts the kernel and runs its
  main loop, each board's part in its `kernel_board.c`
  (`include/kernel_board.h`), QEMU's in `kernel/qemu_board.c`; the devices
  share the bootloader handoff's checks (`kernel/boot_handoff.c`) and the
  fastboot protocol of their USB links (`kernel/usb/fastboot.c`).
- `boards/redmi2`: the Redmi 2's drivers, their headers and its linker script.
- `boards/gta7lite`: the Galaxy Tab A7 Lite's drivers: bootloader handoff,
  watchdog, PMIC, touch, eMMC, USB and Wi-Fi.
- `boards/pc` and `boot/uefi`: a PC's drivers (PCI, xHCI with USB keyboards,
  mice and the stick, Intel's 82574L, HD Audio, ACPI's power button) and
  SOS2's UEFI loader.
- `boards/nexus5`: the Nexus 5's drivers: bootloader handoff, display,
  backlight, touch, PMIC, battery, vibrator, the other cores and the CPU
  clocks, USB, the eMMC, Wi-Fi's SDIO with the BCM4339's firmware, and its
  linker script.
- `boards/qcom`: drivers the Qualcomm phones share: the QUP I2C controller,
  the SD host controller (the eMMC on both, Wi-Fi's SDIO on the Nexus 5) and
  the secure monitor calls (SCM).
- Each board's directory is on the include path only for that board's build;
  shared code includes its headers inside `#ifdef SOS2_BOARD_*`, and asks
  what kind of board it runs on through the family macros `tools/build.mk`
  defines (`SOS2_DEVICE`, `SOS2_PHONE`, `SOS2_LOADER_FRAMEBUFFER`,
  `SOS2_NAVIGATION_BUTTONS`, `SOS2_CPU_SLEEP`).
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
- [Redmi 2](docs/redmi2.md): booting, Wi-Fi, installing, updating and the
  hardware.
- [Galaxy Tab A7 Lite](docs/gta7lite.md): the AArch64 port's boot image and
  the tablet's hardware.
- [Nexus 5](docs/nexus5.md): booting, installing, Wi-Fi, the USB link and
  the hardware.
- [x64](docs/x64.md): the x64 system in QEMU's microvm and on a
  PC: its stick, loader and drivers, and what real PCs still need.
- [Roadmap](docs/roadmap.md): open work.
