# Architecture

SOS2 is a native ARM32 kernel with the desktop and drivers built in. Apps are
web pages, plus [native apps](#native-apps). The system's own programs are the
engine, `/bin/sos2-web`, which runs once per open page, and the
[updater](#updates), `/bin/sos2-updater`.

## Processes and CPUs

- Each page or app is a static ARM ELF process in its own address space. The
  kernel preempts user code every 5 ms; a fault ends only that process, and
  **Close** reclaims even a hung page.
- Free RAM and, per process, a 192 MiB quota and 16 threads limit what runs.
  When the last page closes, free memory must be back at its post-boot
  baseline, or the kernel panics.
- Up to four CPUs run. CPU 0 owns the desktop, scheduler, syscalls and
  drivers; the others run page threads. On the Redmi 2 the secondary cores
  start as the vendor kernel starts them, and a core that misses a bounded
  wait is skipped.

## Memory

Page permissions protect kernel and device memory, keep code read-only and
never allow writable executable mappings; stacks have guard pages, syscalls
validate every user buffer, and JIT is disabled. Devices never DMA into user
memory. Read-only ELF segments (the engine's code and constants) are loaded
once and shared by every page. The engine's allocator is mimalloc, configured
for SOS2, which backs every mapping with RAM when it is created.

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
  whole disk: `build/native-arm32/data.img` in QEMU, a
  [dedicated partition](redmi2.md#persistent-storage) on the phone. The kernel
  never formats it. While QEMU is stopped, `build/host/fs-image` reads and
  writes a profile's files, and `tools/web_content.py` installs the local
  website.
- **Installed mode** (`sos2.storage=installed`) is persistent mode on an
  [installed](redmi2.md#installing) phone, whose volume follows its app store
  in the cache partition. The kernel formats it when it does not mount, and
  runs as a RAM session when the eMMC fails. Removing an app deletes its
  files.

Web storage and cookies follow the usual browser rules (localStorage per
origin, no third-party cookies, the pinned Public Suffix List) within fixed
limits: 5 MiB per origin and a 12 MiB database.

## Updates

The updater downloads `sos2-redmi2.update` and app packages from GitHub Pages
and hands them to the kernel's update service
(`kernel/protection/system_update.c`) with `SYS_UPDATE`, which only it may
call. Like the browser it is part of the system: the desktop's **UPDATES**
button opens it, and no launcher lists it. On a phone started from its
installed system, the kernel checks each file, writes a system update into
the cache slot the boot image does not use and the boot image last, and an
app into the half of its app store position not in use, so an interrupted
install leaves what ran before ([details](redmi2.md#updates)). QEMU and RAM
sessions show updates but install none.

## Networking

lwIP provides IPv4, DHCP, DNS and TCP over virtio Ethernet (QEMU) or the
native Wi-Fi driver (Redmi 2). HTTPS (Mbed TLS 1.2/1.3) checks hostname,
chain and date with no fallback to HTTP; WebKit enforces CORS, CSP, mixed
content and credentials. HTTP/1.1 and HTTP/2 are supported; bodies are
limited to 8 MiB.

## Web platform and rendering

JavaScriptCore runs its interpreter without a JIT; dedicated workers are
supported. Not available: IPv6, WebSocket, file uploads, IndexedDB, service
workers, media and WebGL/WebGPU. Text uses FreeType, HarfBuzz and ICU with
DejaVu, Noto Sans CJK and Noto Color Emoji.

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

The power button toggles standby (QMP `system_powerdown` in QEMU). Standby
turns the screen off and freezes every page; kernel services and the network
keep running. On the phone each device powers down as Linux and the vendor
kernel do it: the display pipeline and panel, the touch controller, Wi-Fi's
radio, the GPU, the secondary cores (the cluster slows to 200 MHz), and the
whole chip between 1 s polls of the power key, with the PMIC rails lowered
through the RPM. Standby draws 6.0 mA, about two weeks on the battery.

## Time

A small SNTP client sets UTC after DHCP and hourly, from Cloudflare's time
service or `sos2.ntp=host[:port]`. Replies must match a random cookie, but
SNTP is not authenticated. Monotonic timers never jump, and there are no time
zones. With persistent storage, an RTC calibration keeps the time across
reboots.
