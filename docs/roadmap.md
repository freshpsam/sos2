# Roadmap

Open work; the other documents describe what exists.

1. **Handset qualification.** Browse complex sites (forms, scrolling, several
   pages, **Close** recovery, memory pressure, long sessions), measure startup
   and page-load times, and check multicore behavior and battery readings,
   which QEMU cannot model. Qualify the [installed system](redmi2.md#installing):
   the eMMC handoff, the boot-time read, an update's writes and the restart.
2. **Wi-Fi security and recovery.** Test WPA2 and SAE, protected management
   frames, key renewal, wrong passwords and reconnection across routers; then
   add radio interrupts.
3. **Storage durability.** Validate power loss and durability of the
   installed systems' eMMC data. Multi-block reads, which the Nexus 5's
   driver uses, would shorten the Redmi 2's boot-time read, which still
   reads a sector at a time.
4. **Compositing and GPU drawing.** Composited fixed and overflow-scrolling
   elements, masks and blend modes in the CPU compositor, which paints the
   last two flattened where they change; damage tracking and worker-thread
   tiles in the GPU compositor; fewer device requests per frame with virgl;
   qualify the [Adreno path](gpu.md#redmi-2-adreno), then bring GPU drawing to
   the Adreno 306 through freedreno.
5. **A 64-bit device.** The AArch64 system runs JavaScriptCore's baseline and
   DFG JIT in QEMU; the Redmi 2 cannot leave 32-bit mode. The
   [Galaxy Tab A7 Lite](gta7lite.md) starts it from its boot partition,
   engine included, with touch, the eMMC, CPU-jitter randomness, the clock and
   Wi-Fi, over which pages load and it updates itself; in standby its other
   cores power down. Next there: the chip asleep in standby, through
   MediaTek's power manager.
   Then WebAssembly, and with it the FTL tier,
   whose B3 backend WebKit builds only with WebAssembly; and memory committed
   on first touch, as JavaScriptCore maps its JIT pool, and on AArch64 its
   structure heap, up front.
6. **Updates and time.** A shorter boot-partition write when updating
   (multi-sector eMMC writes), and a way back when a release fails to start,
   which now takes lk2nd's fastboot; authenticated time and time zones.
7. **Web platform.** IndexedDB, service workers, Cache Storage, downloads,
   media files outside Media Source Extensions, more codecs (VP9, Opus),
   Web Audio and other web APIs; WebGL once the devices draw with their GPUs;
   website-data controls and per-site process isolation, since every page
   shares the browser profile.
8. **Sound.** On the Redmi 2, headphones (jack detection through the
   PM8916's MBHC and the jack's switch, GPIO 8); on the tablet, its volumes
   kept across restarts, its headphones' microphone and buttons; on both,
   the codecs' power-up and power-down sequences (about 25-30 ms each) run
   in steps rather than holding CPU 0's loop. In the browser, sound for more
   than one element at a time (a second video plays silently), and volume
   and mute that reach the half second already queued, which needs
   `SYS_AUDIO` to take them.
9. **Power.** Standby draws 6.0 mA. The tablet and the Nexus 5 raise their
   CPU clocks only while pages work, the Nexus 5 within what its cores'
   temperature allows. Next: Wi-Fi's share in power save, the display's
   power domain, the Nexus 5's cores below their 1497.6 MHz floor while
   idle, sleep with the screen on, and a temperature watch on the tablet.
10. **Speed.** The kernel's mutable data is uncached, apart from the network
    stack's and the audio mixer's, which slows CPU 0's loop (about 22 µs a
    turn on the Nexus 5 while YouTube plays), and every syscall waits for
    it; DMA buffers in each board's BSS have to move out first. Syscalls
    wait longest behind CPU 0's long steps (YouTube's waited 14 µs on
    average, at most 2.3 ms): a full redraw scales the whole screen at once
    (about 6 ms), and a socket call can copy for up to 3 ms. CPU 0 also scales each page frame to the panel, 1.4 ms of a video
    frame on the Nexus 5 (in bands), mostly two loads a pixel building rows;
    the display engines' own scalers would take that off it. In the browser, a playing video's frames
    are still blended with the page's layers above them, and filters are
    painted with their layer, so a blurred element is blurred again whenever
    something near it repaints; composited filters, kept once blurred, would
    spare that. The first page after boot waits about 1.3 s on
    the Nexus 5 while it reads the durable database from the eMMC (4 KiB a
    command), and pages re-download and re-parse every script: an HTTP cache
    shared by pages and a bytecode cache would spare both (parsing and
    generating bytecode take a quarter of YouTube's main thread). The
    Nexus 5's Wi-Fi brings YouTube's 3.4 MB in about 1.3 s.
11. **Nexus 5.** The [Nexus 5](nexus5.md) runs installed, its data in the
    cache partition, or from RAM, on four cores at 1497.6 MHz and up to
    2265.6 MHz while pages work (RPM vote, temperature watch), with touch,
    Wi-Fi (open and WPA2 on 2.4 GHz, power saving), the battery, the
    vibrator and standby (backlight, panel, radio). Next there: the cores'
    power-down in standby (TrustZone's power collapse through each core's
    SPM); Wi-Fi's 5 GHz and speed (an update downloaded
    at about 0.2 MB/s before the driver read glommed frames, not measured
    since); an update's boot-partition writes, still a sector at a time
    (about 0.5 MB/s; the data volume's go 8 sectors a command, and reads up
    to 256); the Wi-Fi chip's own sleep
    in standby (KSO), once the phone's standby current can be measured; the
    Adreno 330; sound beyond the loudspeaker (earpiece, headset jack,
    microphones).
12. **PCs.** The [x64](x64.md) system starts from a USB stick through
    its own UEFI loader on QEMU's emulated PC (q35 with OVMF): the desktop in
    the middle of the monitor, USB keyboards and mice, Intel's 82574L, HD
    Audio, ACPI's power button, data on the stick and updates of its system.
    Next, real PCs: Secure Boot, more network cards (Intel's I225 first),
    NVMe and AHCI and installing onto them, real HD Audio codecs, USB hubs,
    sticks larger than the image, split page tables (KPTI) on CPUs that
    Meltdown affects; then a desktop laid out for monitors.
