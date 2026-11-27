# Architecture

SOS2 is a native kernel with the desktop and drivers built in, for ARM32
(QEMU, the Redmi 2 and the Nexus 5), AArch64 (QEMU and the
[Galaxy Tab A7 Lite](gta7lite.md)) or [x64](x64.md) (QEMU's microvm,
and PCs, so far QEMU's), chosen at build time. Apps are
web pages, plus [native apps](#native-apps). The system's own programs are the
engine, `/bin/sos2-web`, which runs once per open page, and the
[updater](#updates), `/bin/sos2-updater`.

## Processes and CPUs

- Each page or app is a static ELF process in its own address space. The
  kernel preempts user code every 5 ms; a fault ends only that process, and
  **Close** reclaims even a hung page.
- Free RAM and, per process, a 544 MiB quota (1 GiB on the 64-bit systems) and 64
  threads limit what runs. A page left with under 64 MiB, of its quota or of
  free RAM, has WebKit release its caches, decoded images and compiled code
  and collect garbage, again only once it has grown by 32 MiB or has under
  16 MiB left (`programs/browser/web_main.cpp`). Memory a page frees stays
  its own, and a page whose allocation fails stops, most often the one in
  use. So with under 64 MiB of RAM free the web page shown least recently,
  a silent one first, never the one on screen nor an app, is parked: its
  process ends and gives its memory back, and its entry stays in the
  desktop's list, darker, starting the page again at its address when opened
  (`kernel/protection/page_host.c`). Pages are parked one at a time, none
  while a stopped page's memory is still coming back, and never one whose
  address was too long to keep. When the last page closes, free memory must
  be back at its post-boot baseline, or the kernel panics.
- Up to four CPUs run, eight on the 64-bit systems. CPU 0 owns the desktop, scheduler,
  syscalls and drivers; the others run page threads, a page's own threads
  side by side (its GC markers, compiler threads, image decoders and
  workers). Only unmapping or protecting memory and suspending a thread
  pause the page's other threads. Between the longer steps of its loop
  (input, drawing, the network, the service worker) CPU 0 takes the calls
  and time slices the other cores have finished and starts their next
  threads: YouTube's calls wait 14 µs for it on average on the Nexus 5. On
  the Redmi 2 the secondary cores start as the vendor kernel starts them,
  and a core that misses a bounded wait is skipped.

## Memory

Page permissions protect kernel and device memory and keep code read-only.
Writable memory is never executable, except in the engine's JIT window (32 MiB
on ARM32, 64 MiB on the 64-bit systems), which only page processes may map.
JavaScriptCore maps its whole JIT pool there when a page starts (4 MiB on
ARM32, 16 MiB on the 64-bit systems), writes its code into it and then
synchronizes the instruction cache (`sos2_icache_sync`). AArch64 user code
does that itself, and x64's needs nothing; ARM32 user code can't, so a
system call does it on the calling CPU, which already has the page's
mappings, and returns straight to the page.
User memory and the kernel's code, constants and private data are
write-back and inner shareable, as Linux sets ARMv7 and AArch64 up. ARM32
gets inner shareable through TEX remap (`include/arm_cache.h`): without it the
shareable bit means outer shareable, which the Nexus 5's Krait cores keep out
of their L1 data cache, so every load went to the L2 and JavaScript ran ten
times slower. The kernel's other mutable data is uncached, since devices read
some of it, except the page allocator's, the network stack's (`kernel/net`,
`kernel/wifi`, lwIP and the Nexus 5's Wi-Fi driver, which moves data by PIO)
and the audio mixer's: only CPU 0 uses it, so the linker scripts place it
with the private data. On x64, whose devices' DMA is coherent, all RAM is
write-back ([x64](x64.md#memory)). Free pages are zeroed through their uncached alias in
32-byte bursts, 5.8 GB/s on the Nexus 5 against 1.5 for word stores. Translation tables are cached too, and the MMU walks them
through the caches, so a TLB miss costs no trip to DRAM: layout-heavy work ran
36% faster on the Nexus 5.
Stacks have guard pages, and kernel faults run on stacks of their own, so a
kernel stack overflow is still reported. Syscalls validate every user buffer,
and devices never DMA into user memory. 64-bit user space lies above 4 GiB.
Read-only ELF segments (the engine's code and constants) are loaded once and
shared by every page. The engine's isn't even copied: the packager starts it
on a megabyte of the resources (`tools/package_redmi2.py`), and the kernel
maps it where it lies. ARM32 maps that block with 1 MiB sections, as it does
every whole aligned megabyte of a big private mapping (the heaps', the JIT
pool's) that finds a run of free pages, taken from the pool's top, away from
single pages; the 64-bit systems map the shared block with 2 MiB blocks. With 4 KiB
pages alone the Nexus 5 spent much of its time walking tables. A private
section keeps its page table aside, in step, and whatever changes part of it
(protecting, unmapping) turns it back into that table first
(`arch/arm/mmu.c`). The engine's allocator is mimalloc, configured for SOS2,
which backs every mapping with RAM when it is created. JavaScriptCore cuts
its 16 KiB heap blocks from 1 MiB chunks of its own, which get sections too,
and unmaps a chunk that empties while another stays empty
(`wpe-2.52.6-native-blocks.patch`): mapped one by one, YouTube's blocks took
some 5,500 system calls a load. Both ask mmap for
aligned memory (`MAP_ALIGNED`), which the kernel places directly, so no
slack is unmapped.
A mapping or unmapping over 1 MiB doesn't hold the kernel's loop, as 60 MiB
did for 45 ms on the Nexus 5: while its thread waits, the kernel first claims
the range (promising its pages, or retiring its translations while the
process is quiescent), then maps or frees 32 pages a turn of the loop
(`kernel/protection/process.c`); an executable one advances the epoch for
each page it fills.

On AArch64 the device tree places everything: the kernel's page pool is the
16 largest stretches of RAM below 4 GiB that no reservation, framebuffer,
device tree or resource uses, with the gaps between them withheld, and the
resources arrive as the initrd. A compressed
(SORZ) one, as on the tablet, expands at boot into the smallest free stretch
that holds it, through a temporary cached mapping (`arch/arm64/boot_cache.c`),
since the data cache is still off. On x64, QEMU's PVH start information
and ACPI do ([x64](x64.md#boot)).

## Services and SDK

Filesystem, entropy, radio, update and page GPU requests run on one
serialized kernel service worker that yields to the desktop, and while
reading files at least every 2 ms. Only the calling thread waits for its
request: the process's other threads run on, and may change their mappings
meanwhile, since the worker copies to and from a process's memory only
between its yields, walking the page tables for each copy. (A browser
database save, seconds of eMMC writes, used to stop every thread of its
page, its video too.) The SDK (`libsos2.a`, headers in
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
| `/data/browser/saved/` | Cookies and web storage, as last saved (packs and a manifest) |
| `/tmp/browser/state.sqlite3` | Cookies and web storage as pages use them |

Web pages share the profile `/profiles/browser`; the updater has
`/profiles/updater` and each app `/profiles/apps/<id>`. The kernel separates
profiles and WebKit separates origins; nothing is encrypted at rest. Pages
share the browser's database through kernel file locks taken by SQLite's VFS
(`sdk/runtime/sqlite_vfs.c`), and pick up each other's cookie and storage
changes within 250 ms. That database lives in `/tmp`, a 24 MiB RAM volume
emptied at every boot, whose file system writes in place
(`kernel/protection/ramfs.c`): a commit to a 3.5 MB database takes about
7 ms on the Nexus 5. littlefs, which `/data` keeps, rewrites a file from its
first changed byte to its end, and SQLite changes a database's first page
at every commit: in littlefs a commit took 330 ms, and on the eMMC it was
half of a heavy site's load. SOS2's littlefs
(`third_party/littlefs/upstream.json`) copies that tail in 512-byte pieces,
not a byte at a time, and once a write: a later write further on copies only
what lies between, so a commit, whose pages go in ascending order, copies
the file once. littlefs keeps a block (4 KiB) in each cache, so it programs
data a block at a time, which the Nexus 5's driver writes in one transfer
(CMD25, about four times as fast as a sector at a time), and SOS2's copy
checks what it programmed 512 bytes at a time rather than 8. Its allocator
looks ahead 32768 blocks (128 MiB): it fills each window by reading every
file's block list, thousands of sector reads on the eMMC. To find each next
block of a file it reads a few words from some ten blocks of its skip list,
so the kernel keeps the volume's blocks as last read, 1 MiB direct-mapped by
number (`kernel/protection/fs.c`): the first page after boot read 12 MB in
8,300 commands and 2.6 s on the Nexus 5, about twice what it takes now. In
`/tmp`, a file removed or replaced stays readable through the descriptors
open on it, as in POSIX; in `/data` they fail with EIO. The first page after
boot copies the durable database's bytes from `/data`, and changes go back
3 s after the last
change, or 30 s after the first unsaved one, saved by one page at a time:
the page that made the change, or any page open later
(`programs/browser/web_state.cpp`). A thread of the page's own takes the
working copy's bytes inside an SQLite read transaction, RAM to RAM, then
writes what changed while the page goes on. The durable copy is the
database in 32 KiB pieces, kept as packs, a file for each save holding the
pieces it wrote, and a manifest with each piece's pack, place and hash: a
save writes the pieces whose bytes changed to a new pack, renames a new
manifest over the old one, then removes the packs it no longer names; when
more than four would stay in use, it writes every piece to its pack. Sites
keep megabytes there (Wikipedia's module cache: 2 MB): a save of YouTube's
few changed bytes, which rewrote the whole database (5 s on the Nexus 5),
now writes two pieces, and one of 1.5 MB takes about a second, where a file
for each piece took ten, most of it in littlefs's directory commits. A
power loss or restart loses at most those last seconds, unless no page
stayed open after the page that made a change closed.

- **RAM mode** (`sos2.storage=ram`, the default for `make run` and
  `make phone`) keeps a 32 MiB data volume in RAM until reboot.
- **Persistent mode** (`sos2.storage=disk`) uses one littlefs volume over the
  whole disk: `build/native-arm32/data.img` (or `native-arm64`) in QEMU, a
  [dedicated partition](redmi2.md#persistent-storage) on the phone. The kernel
  never formats it. While QEMU is stopped, `build/host/fs-image` reads and
  writes a profile's files, and `tools/web_content.py` installs the local
  website.
- **Installed mode** (`sos2.storage=installed`) is persistent mode on an
  installed [Redmi 2](redmi2.md#installing),
  [tablet](gta7lite.md#updates-and-apps) or [Nexus 5](nexus5.md#installing),
  whose volume follows its app store in the cache partition. The kernel
  formats it when it does not mount, and runs as a RAM session when the eMMC
  fails. Removing an app deletes its files.

Web storage and cookies follow the usual browser rules (localStorage per
origin, no third-party cookies, the pinned Public Suffix List) within fixed
limits: 5 MiB of storage, per origin and in all, in a database of at most
6 MiB.

## Updates

The updater downloads the device's update (`sos2-redmi2.update`,
`sos2-gta7lite.update` or `sos2-nexus5.update`, by the board the kernel
names) and app packages from GitHub Pages and hands them to the kernel's update service
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
devices' native Wi-Fi drivers. The devices share one station (kernel/wifi),
which builds and parses 802.11 and runs WPA2/WPA3; the Nexus 5's FullMAC
firmware joins by itself, so its radio tells the station what the firmware
did through the station's FullMAC calls (authenticated, associated,
disconnected, heard: include/wifi_station.h;
[details](nexus5.md#loader-and-hardware)). TCP keeps segments that arrive
past a gap and reports them with SACK, so a frame Wi-Fi loses costs one
resend rather than a window; the Wi-Fi drivers hand lwIP up to 256 received
frames at a time, a whole aggregate. HTTPS (Mbed TLS 1.2/1.3, its bignum
arithmetic in the CPU's own multiply-accumulate) checks hostname, chain and
date against the Mozilla roots, kept as DER (`/certs/roots.der`), with no
fallback to HTTP; WebKit enforces CORS,
CSP, mixed content and credentials. HTTP/1.1 and HTTP/2 are supported;
bodies are limited to 32 MiB decoded. HTTP/2 offers each stream a 1 MiB
window and the connection 3 MiB (`programs/browser/http2_session.h`), so a
server sends a big script without waiting for window updates: with them,
and with reads of up to 128 KiB a turn, YouTube's 3.4 MB arrives on the
Nexus 5 in about 1.3 s rather than 2.2. Each page reads, decrypts and decodes
on a network thread of its own (`SOS2::HTTP::Fetch` in
`programs/browser/http_client.cpp`), holding up to 4 MiB of a response for
the main thread, which takes it at its next turn: through a long script or
layout the sockets would otherwise go unread until TCP's window closed.

## Web platform and rendering

JavaScriptCore compiles page scripts with its baseline and DFG JIT tiers on
all three architectures. SOS2 pins WPE WebKit 2.52.6 to retain the ARMv7 JIT and
backports selected upstream security fixes. Each security patch in
`tools/browser/patches/` identifies its upstream commit. A performance
patch of SOS2's own (`wpe-2.52.6-native-jsc-profiles.patch`) keeps the LLInt
from updating all of a function's value profiles at every loop back edge and
call while the function's baseline code compiles, and reads each profile
bucket once, without barriers, when profiles are updated. Another
(`wpe-2.52.6-native-first-paint.patch`) has the HTML parser of a visible
main frame wait for a rendering update, up to 100 ms, before each of its
first three blocking scripts that are external or long, once it has parsed
something since it last yielded: what comes before a long script shows
before it runs, as YouTube's skeleton does. The
`sos2.jsc=` boot argument (`RUN_ARGS='--jsc ...'` in QEMU, `REDMI2_JSC` and
`GTA7LITE_JSC` for the devices) passes JavaScriptCore options to every page, such as `useJIT=false` to
compare the tiers. Dedicated workers are supported.
Every device presents itself as a mobile browser, Safari on an iPhone
(`programs/browser/web_navigation.cpp`), so sites send their mobile pages,
which the devices load in about half the time: on the Nexus 5 at 1.5 GHz
YouTube's showed content after 6 to 7.5 s, its desktop page after 12 to
14 s, in 154 MiB rather than 396. Some sites then suggest their app, and a
site that recognizes iOS may offer video as HLS, which the engine does not
play.
Not available: IPv6, WebSocket, file uploads, IndexedDB, service workers,
Web Audio and WebGL/WebGPU; video plays only through Media Source Extensions
([Video](#video)). Text uses FreeType, HarfBuzz and ICU with DejaVu and Noto
Color Emoji. SOS2's language is English: ICU's data keeps its own alone
(tools/build_native_runtime.py), DejaVu's upright faces are cut down to
Latin, Greek and the common symbols, which WebKit slants for italics, and the
emoji font to all but flags (tools/browser_fonts.py); other scripts draw
nothing. So the system, engine included, fits each device's boot partition,
the Nexus 5's 22 MiB the smallest: 22.1 MB compressed there, 23.3 MB on the
Redmi 2 and 24.3 MB on the tablet, with the engine built at -O2 and
position-dependent (WebKit's default, position-independent code, costs
ARM32 a register and a load at each use of a global, and 0.6 MB), and its code
through branch filters (include/boot_bundle.h), which make call targets
absolute: xz's ARM64 one on AArch64, and on ARM32, whose engine is Thumb-2
but for a little assembly, an ARM and a Thumb-2 one, each over its
instruction set's ranges.

WebKit draws with Skia on the CPU into a retained backing store per page, or
with Skia's GL backend under QEMU [virgl](gpu.md). Scrolling moves pixels and
repaints only what is exposed. Animated layers (transforms, opacity) and
videos go to a CPU compositor that redraws only what changed
(`programs/browser/web_compositor.h`), and nothing beneath a layer that
covers the change. It keeps pixels for the layers the viewport shows, up to
24 MiB, and draws each video frame into the page itself, so playback repaints
nothing else. A thread of the page's own, its video presenter
(`programs/browser/web_main.cpp`), shows a playing video's frames while the
main thread runs scripts: where the last composite left the video, unscaled
at whole pixels, it copies each new frame, blends the layers above it there
back over it, and presents that rectangle. A composite that draws anything
else into the rectangle draws those layers again first. YouTube's watch page
keeps the Nexus 5's main thread 90% busy, which showed a few frames a
second; the presenter shows its 25. Layers the compositor cannot draw
(masks, blend modes, more than the budget) are painted flattened where they
changed. Fixed elements that overlap layers become layers too and keep their
place as the page scrolls
(`tools/browser/patches/wpe-2.52.6-native-fixed-layers.patch`). Pages send
the kernel only changed pixels. CSS and SVG filters are painted with their
layer, so a blurred element is blurred again whenever something near it
repaints. A blur whose deviation is above 8 pixels along an axis runs there
at reduced resolution (to a deviation of 4) and is drawn back enlarged
(`tools/browser/patches/wpe-2.52.6-native-blur.patch`): YouTube's mobile
player blurs its poster 66 pixels wide over 1240×1044 pixels, which took
250 ms on the Nexus 5 at each repaint and 6% of the main thread while the
page loaded; reduced it takes 0.3%.

## Desktop and input

The desktop draws a 480×800 screen, scaled to the phone's panel: a 48-pixel
header above a 480×752 page surface. Where CPU 0 scales it (all but the
Redmi 2, whose Adreno presents), a page's new frame is copied into the screen
at once and then scaled a band of 32 rows a turn of CPU 0's loop (about
0.3 ms each on the Nexus 5), and shown once whole: a video frame took 1.8 ms
in one step before, holding every system call.

- The desktop shows status lines, **WI-FI** and **UPDATES**, the address
  field with **Open**, the open pages and, at the bottom, the **Apps**
  launcher, with room for all ten apps in two rows of five.
- A page's header has **Close**, the UTC time and the battery, whose icon
  shows its percentage, or a plus while it charges. Home (Alt+Home
  in QEMU) shows the desktop from a page, an app or the Wi-Fi screen. Back
  (Alt+Left in QEMU) goes back within what is shown, never to the desktop: in
  a page's history, in an app only if the app takes it (`sos2_back_key`), and
  on the Wi-Fi screen from a network to the list.
- Dragging scrolls; a movement of up to six pixels is a tap. On the phone,
  buttons and keys give a 15 ms buzz (`kernel/protection/haptics.c`).
- Pages hear the finger as a phone's browser tells them, as the iPhone user
  agent promises: touchstart when it lands, touchmove as it moves, touchend
  when it lifts, then a tap's mouse events and click
  (`programs/browser/web_input.cpp`). A page that cancels touchstart, or a
  touchmove before the drag scrolls, keeps the drag from scrolling;
  cancelling touchstart or touchend drops the click. YouTube's mobile player
  starts a video with sound only after a touchstart.
- Header, keyboard and desktop input, including Wi-Fi passwords, never
  reaches a page.

## Video

Pages play video through Media Source Extensions, as YouTube and most
streaming sites send it: fragmented MP4 with H.264 (8-bit Baseline, Main or
High profile) and AAC (LC or HE). SOS2's media engine
(`tools/browser/native-port/Source/WebCore/platform/graphics/sos2`) parses
the segments and decodes each track on a thread of its own with FFmpeg's
H.264 and AAC decoders, the only parts of FFmpeg built
(`tools/build_native_render.py`, LGPL 2.1); H.264 decodes frames in parallel
on up to four more threads. Each frame is converted on its decoder thread to
the size the page shows it at, so the compositor copies it unscaled: scaled
in YUV first (bilinear, eight pixels at a time in NEON: `YUVScalerSOS2`), then
turned into RGB unscaled, which FFmpeg does in NEON (scaling straight into RGB
runs its C code: 70 ms a 480p frame on the Nexus 5). A 480p frame shown 1080
pixels wide takes about 2 ms to scale and 2 ms to convert there. It
then waits for the clock: the page's [audio stream](#audio) as it is heard, or
monotonic time without audio. Decoders hold about three seconds of samples
(four of AAC): source buffers refill them on the main thread, which a page's
scripts may hold most of a second. A page has one audio stream, which its
first video with sound takes. Pages may start playback, sound included,
without a tap. They are told that video larger than 1280×720 (854×480 on
ARM32) or faster than 30 frames per second is unsupported, which keeps
YouTube's choice of quality to what the devices decode, and each source
buffer keeps at most 64 MiB (24 MiB on ARM32). Media files played without
MSE, other codecs and playback rates other than 1 are not supported.

## Audio

A page or app may open one output stream (`sos2_audio`, `SYS_AUDIO`) of
interleaved 16-bit stereo frames at 48 kHz. Writes never block: the kernel
queues up to 0.5 s per stream, in pool pages taken when the stream opens and
returned when it closes or its process ends. The mixer
(`kernel/audio/audio.c`) sums the playing streams and saturates the total. A
stream's `played` count grows as frames leave its queue and `latency` counts
those still queued in the device, so what is heard is `played - latency`, the
clock media playback follows. Without an audio device (QEMU without one, or a
device whose driver has not started or has failed), streams play silently at
the same rate, so the clock still keeps real time.
QEMU's virtio-sound device gets 10 ms periods, at most four at a time, and
stops a quarter second after the streams run dry
(`kernel/audio/virtio_snd.c`). The tablet's speakers, or headphones in its
jack, play a ring the audio DMA reads, which the main loop's polls fill up
to 133 ms ahead, with silence where the streams run dry; they power down 3 s
after the last frame. Its volume keys set a volume for each output, which
scales the mix (`boards/gta7lite/audio.c`, [Galaxy Tab A7 Lite](gta7lite.md)).
The Redmi 2's loudspeaker plays the mix in mono from a 256 ms ring in RAM
that the audio interface's DMA reads, filled up to 245 ms ahead, with the
same volume steps (`boards/redmi2/audio.c`, [Redmi 2](redmi2.md#sound)); the
volume steps and the keys' timing are shared (`audio_mix_volume`,
`kernel/volume_keys.c`). The Nexus 5's loudspeaker plays the mix in mono
through the audio DSP, which holds up to 240 ms of 20 ms periods from a ring
in RAM, with the same volume steps (`boards/nexus5/audio.c`,
[Nexus 5](nexus5.md#sound)); its driver's slow start, the DSP's firmware
read from the eMMC, runs on the service worker (`audio_device`'s
`pending`/`service`). `make run` plays through PulseAudio or
PipeWire when one runs; `RUN_ARGS='--audio none'` leaves the device out, and
`'--audio wav:FILE'` records it.

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

The kernel's clock (`time_us`) converts the generic timer's count (x64's
TSC, shifted right by 4) exactly,
multiplying by the frequency's reciprocal, since ARM32 has no 64-bit
division. Reading the count costs 0.3 µs on the Nexus 5, so CPU 0's main
loop reads it once a turn (`loop_time_us`), and the loop's periodic checks
compare deadlines with that reading: the network poll and lwIP's clock, the
audio mixer's silent clock (its system calls run in the loop too), touch,
the power key, the status bar and the background work's turns. Scheduling
and the display read the clock themselves.
A small SNTP client sets UTC after DHCP and hourly, from Cloudflare's time
service. Replies must match a random cookie, but SNTP is not authenticated. Monotonic timers never jump, and there are no time
zones. With persistent storage, an RTC calibration keeps the time across
reboots.
