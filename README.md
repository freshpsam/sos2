# SOS2 (𝚂mall Oper𝚊ting Syste𝚖 v2)

SOS2 is a web-first operating system: apps are web pages, plus native apps
such as a calculator. Native WebKit (WPE WebKit 2.54.2, built as the `SOS2`
port) runs as one process per page on SOS2's own ARM32 kernel, with no Linux.
It runs in QEMU and on the Xiaomi Redmi 2 (MSM8916, WT88047).

The desktop is the browser shell: **WI-FI** and **UPDATES** buttons, an
address field with **Open** above the open pages, an **Apps** launcher and a
touch keyboard. Each page or app is an isolated process. A page's header has
**Desktop**, **Close**, the UTC time and the battery; the phone's Back key
(Alt+Left in QEMU) goes back and the power button toggles standby.

Pages run HTML, CSS, JavaScript, images, CJK text and emoji over verified
HTTPS, with cookies and web storage, drawn by Skia on the CPU (or the GPU with
[virgl](docs/gpu.md) in QEMU). The Redmi 2 browses over Wi-Fi with its data in
RAM, lasts about two weeks in standby, and can be
[installed](docs/redmi2.md#installing) on its eMMC, where the updater installs
new releases and [apps](docs/redmi2.md#apps) from
[GitHub Pages](https://freshpsam.github.io/sos2/).

## Build and run

Needs Linux with Python 3.12+, GNU Make, `arm-none-eabi-` GCC/G++ with C++23
(GCC 14.2), `qemu-system-arm` 10.2+, `dtc` and a host C compiler; the engine
also needs CMake, Ninja, Meson, Ruby, Perl with JSON::PP, gperf, Bison, flex,
Python's Mako, pkg-config and patch.

```sh
make          # QEMU kernel and filesystem tools
make fetch    # download pinned engine dependencies and Adreno firmware
make engine   # build the runtime, WebKit, the updater, the apps and the bundle
make run      # boot the desktop in QEMU with a temporary RAM session
make phone    # build the phone's boot.img, installed system and app packages
make publish  # push the installed system and apps to the gh-pages branch
```

Only `make fetch` downloads, and nothing touches a phone. `make help` lists
the options, such as `STORAGE=persistent` (a disk image in QEMU),
`ENGINE=path/to/native-web.bundle`, `JOBS=8` and `RUN_ARGS='--cpus 1'` or
`'--graphics virgl'`. `make clean` deletes `build/`, including downloads and
the persistent QEMU disk.

## Source map

- `arch/arm`, `kernel`, `include`: the kernel and the desktop.
- `boards/redmi2`: the Redmi 2's drivers.
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
- [Roadmap](docs/roadmap.md): open work.
