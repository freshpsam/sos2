# Architecture

SOS2 is a native ARM kernel with the desktop and drivers built in, for ARM32
(QEMU and the Redmi 2) or AArch64 (QEMU, and the
[Galaxy Tab A7 Lite](gta7lite.md) in progress), chosen at build time. Apps are
web pages, plus [native apps](#native-apps). The system's own programs are the
engine, `/bin/sos2-web`, which runs once per open page, and the
[updater](#updates), `/bin/sos2-updater`.

## Processes and CPUs

- Each page or app is a static ARM ELF process in its own address space. The
  kernel preempts user code every 5 ms; a fault ends only that process, and
  **Close** reclaims even a hung page.
- Free RAM and, per process, a 192 MiB quota (384 MiB on AArch64) and 16
  threads limit what runs. When the last page closes, free memory must be
  back at its post-boot baseline, or the kernel panics.
- Up to four CPUs run, eight on AArch64. CPU 0 owns the desktop, scheduler,
  syscalls and drivers; the others run page threads. On the Redmi 2 the
  secondary cores start as the vendor kernel starts them, and a core that
  misses a bounded wait is skipped.

## Memory

Page permissions protect kernel and device memory and keep code read-only.
Writable memory is never executable, except in the engine's JIT window (32 MiB
on ARM32, 64 MiB on AArch64), which only page processes may map. JavaScriptCore
maps its whole JIT pool there when a page starts (4 MiB on ARM32, 16 MiB on
AArch64), writes its code into it and then synchronizes the instruction cache
(`sos2_icache_sync`). AArch64 user code does that itself; ARM32 user code can't,
so a system call does it on the calling CPU, which already has the page's
mappings, and returns straight to the page.
Stacks have guard pages, and kernel faults run on stacks of their own, so a
kernel stack overflow is still reported. Syscalls validate every user buffer,
and devices never DMA into user memory. AArch64 user space lies above 4 GiB.
Read-only ELF segments (the engine's code and constants) are loaded once and
shared by every page. The engine's allocator is mimalloc, configured for SOS2,
which backs every mapping with RAM when it is created. JavaScriptCore's 16 KiB
heap blocks are mapped one by one at that size instead, since mimalloc would
spend 32 KiB on each aligned block.

On AArch64 the device tree places everything: the kernel's page pool is the
largest stretch of RAM below 4 GiB that no reservation, framebuffer, device
tree or resource uses, and the resources arrive as the initrd. A compressed
(SORZ) one, as on the tablet, expands at boot into the smallest free stretch
that holds it, through a temporary cached mapping (`arch/arm64/boot_cache.c`),
since the data cache is still off.

## Services and SDK

Filesystem, entropy, radio, update and page GPU requests run on one
serialized kernel service worker that yields to the desktop; the calling
process is frozen until its request ends. The SDK (`libsos2.a`, headers in
`sdk/include/sos2`) is a bounded POSIX subset, not a Linux ABI: Newlib, C++
threads and TLS, clocks, private memory mapping, locks and `poll`. Programs
are static, and what they print goes nowhere. Syscalls are in
`include/user_abi.h`.

## Storage

| Path | Contents |
| --- | --- |
| `/bin`, `/apps`, `/fonts`, `/certs` | Read-only system resources |
| `/system`, `/firmware` | Kernel-only system resources |
| `/settings` | Kernel-only Wi-Fi and clock settings |
| `/data`, `/tmp` | The process's profile: its files and temporary files |
| `/data/www` | The browser's local website, `https://www.sos2.local/` |
| `/data/browser/state.sqlite3` | Cookies and web storage |

Web pages share the profile `/profiles/browser`; the updater has
`/profiles/updater` and each app `/profiles/apps/<id>`. The kernel separates
profiles and WebKit separates origins; nothing is encrypted at rest. Pages
share the browser's database through kernel file locks taken by SQLite's VFS
(`sdk/runtime/sqlite_vfs.c`), and pick up each other's cookie and storage
changes within 250 ms.

- **RAM mode** (`sos2.storage=ram`, the default for `make run` and
  `make phone`) keeps a 32 MiB data volume in RAM until reboot.
- **Persistent mode** (`sos2.storage=disk`) uses one littlefs volume over the
  whole disk: `build/native-arm32/data.img` (or `native-arm64`) in QEMU, a
  [dedicated partition](redmi2.md#persistent-storage) on the phone. The kernel
  never formats it. While QEMU is stopped, `build/host/fs-image` reads and
  writes a profile's files, and `tools/web_content.py` installs the local
  website.
- **Installed mode** (`sos2.storage=installed`) is persistent mode on an
  installed [phone](redmi2.md#installing) or [tablet](gta7lite.md#updates-and-apps),
  whose volume follows its app store in the cache partition. The kernel formats it when it does not mount, and
  runs as a RAM session when the eMMC fails. Removing an app deletes its
  files.

Web storage and cookies follow the usual browser rules (localStorage per
origin, no third-party cookies, the pinned Public Suffix List) within fixed
limits: 5 MiB per origin and a 12 MiB database.

## Updates

The updater downloads the device's update (`sos2-redmi2.update` or
`sos2-gta7lite.update`) and app packages from GitHub Pages
and hands them to the kernel's update service
(`kernel/protection/system_update.c`) with `SYS_UPDATE`, which only it may
call. Like the browser it is part of the system: the desktop's **UPDATES**
button opens it, and no launcher lists it. On a device started from its
installed system, the kernel checks each file, writes a system update, a
whole boot image, into the boot partition with its first sector last, and an
app into the half of its app store position not in use, so an interrupted
download leaves what ran before ([details](redmi2.md#updates)). QEMU and RAM
sessions show updates but install none.

## Networking

lwIP provides IPv4, DHCP, DNS and TCP over virtio Ethernet (QEMU) or the
devices' native Wi-Fi drivers. HTTPS (Mbed TLS 1.2/1.3) checks hostname,
chain and date with no fallback to HTTP; WebKit enforces CORS, CSP, mixed
content and credentials. HTTP/1.1 and HTTP/2 are supported; bodies are
limited to 8 MiB.

## Web platform and rendering

JavaScriptCore compiles page scripts with its baseline and DFG JIT tiers on
both architectures. SOS2 pins WPE WebKit 2.52.6 to retain the ARMv7 JIT and
backports selected upstream security fixes. Each security patch in
`tools/browser/patches/` identifies its upstream commit. The
`sos2.jsc=` boot argument (`RUN_ARGS='--jsc ...'` in QEMU, `REDMI2_JSC` and
`GTA7LITE_JSC` for the devices) passes JavaScriptCore options to every page, such as `useJIT=false` to
compare the tiers. Dedicated workers are supported.
Not available: IPv6, WebSocket, file uploads, IndexedDB, service workers,
media and WebGL/WebGPU. Text uses FreeType, HarfBuzz and ICU with DejaVu, Noto
Sans CJK and Noto Color Emoji. SOS2's languages are English and Chinese: ICU's
data keeps theirs alone (tools/build_native_runtime.py), and the CJK font is
cut down to GB2312's Simplified Chinese (tools/browser_fonts.py), so that the
system, engine included, fits a 32 MiB boot partition on both devices:
about 28 MB compressed on each, with the engine built at -O2 and its code
through branch filters (include/boot_bundle.h), which make call targets
absolute: xz's ARM64 one on AArch64, and on ARM32, whose engine is mostly
Thumb-2 with ARM-mode libraries, an ARM and a Thumb-2 one, each over its
instruction set's ranges.

WebKit draws with Skia on the CPU into a retained backing store per page, or
with Skia's GL backend under QEMU [virgl](gpu.md). Scrolling moves pixels and
repaints only what is exposed. Animated layers (transforms, opacity) go to a
CPU compositor that redraws only what changed, up to a 24 MiB budget. Pages
send the kernel only changed pixels.

## Desktop and input

The desktop draws a 480×800 screen, scaled to the phone's panel: a 48-pixel
header above a 480×752 page surface.

- The desktop shows status lines, **WI-FI** and **UPDATES**, the address
  field with **Open**, the open pages and, at the bottom, the **Apps**
  launcher, with room for all ten apps in two rows of five.
- A page's header has **Close**, the UTC time and the battery. Home (Alt+Home
  in QEMU) shows the desktop from a page, an app or the Wi-Fi screen. Back
  (Alt+Left in QEMU) goes back within what is shown, never to the desktop: in
  a page's history, in an app only if the app takes it (`sos2_back_key`), and
  on the Wi-Fi screen from a network to the list.
- Dragging scrolls; a movement of up to six pixels is a tap. On the phone,
  buttons and keys give a 15 ms buzz (`kernel/protection/haptics.c`).
- Header, keyboard and desktop input, including Wi-Fi passwords, never
  reaches a page.

## Native apps

Native apps are static SDK programs with a 48×48 icon and a name of up to 12
characters (`include/apps.h`). QEMU's development bundle builds them in; on
an installed phone they are packages kept in an app store and installed one
by one ([details](redmi2.md#apps)). The launcher shows up to ten, one
instance each. An app runs like a page, with its own profile; it is built
for an ABI (`USER_ABI_VERSION`) and does not start on another. Sources live
in `apps/<id>/`: `app.json` (the name and any SDK `libraries`), `icon.txt`
(pixel art) and C files, built by `tools/build_native_apps.py`.

The **Calculator** gives multiplication and division precedence, shows
`Error` on division by zero and also takes keyboard input.

## Power and standby

The power button toggles standby (Machine > Power Down in QEMU's window). Standby
turns the screen off and freezes every page; kernel services and the network
keep running. On the phone each device powers down as Linux and the vendor
kernel do it: the display pipeline and panel, the touch controller, Wi-Fi's
radio, the GPU, the secondary cores (the cluster slows to 200 MHz), and the
whole chip between 1 s polls of the power key, with the PMIC rails lowered
through the RPM. Standby draws 6.0 mA, about two weeks on the battery.

## Time

A small SNTP client sets UTC after DHCP and hourly, from Cloudflare's time
service. Replies must match a random cookie, but SNTP is not authenticated. Monotonic timers never jump, and there are no time
zones. With persistent storage, an RTC calibration keeps the time across
reboots.
