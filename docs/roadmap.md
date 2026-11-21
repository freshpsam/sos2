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
8. **Sound.** An audio driver for the phone (the MSM8916's LPASS and the
   PM8916 codec), whose pages until then play video silently, on the same
   clock; on the tablet, its volumes kept across restarts, its headphones'
   microphone and buttons, and the codec's power-up and power-down ramps
   (about 30 ms each) run in steps rather than holding CPU 0's loop. In the
   browser, sound for more than one element at a time (a second video plays
   silently), and volume and mute that reach the half second already queued,
   which needs `SYS_AUDIO` to take them.
9. **Power.** Standby draws 6.0 mA. The tablet raises its CPU clocks only
   while pages work. Next: Wi-Fi's share in power save, the display's power
   domain, the phone's clock and voltage scaling, sleep with the screen on,
   and thermal handling.
10. **Speed.** The kernel's mutable data and page tables are uncached, which
    slows CPU 0's loop (every syscall waits for it) and every TLB miss. In the
    browser, a playing video's frames are still blended with the page's
    overlays above them, and layers with CSS shadows or filters blur them
    again at each repaint: YouTube's player controls, while shown, hold
    playback near 8 frames per second on the tablet. The browser saves its
    cookie and storage database (up to 6 MiB) on the page's main thread,
    which holds the page while the copy is written.
11. **Nexus 5.** The [Nexus 5](nexus5.md) runs installed, its data in the
    cache partition, or from RAM, on four cores at 1497.6 MHz, with touch,
    Wi-Fi (open and WPA2 on 2.4 GHz, power saving), the battery, the
    vibrator and standby (backlight, panel, radio). Next there: the cores'
    power-down in standby (TrustZone's power collapse through each core's
    SPM); faster CPU levels (the RPM's logic rail over SMD, the CPU rail,
    and a temperature watch); Wi-Fi's 5 GHz and speed (an update downloaded
    at about 0.2 MB/s before the driver read glommed frames, not measured
    since); the eMMC's writes (a sector at a time in PIO, about 0.6 MB/s;
    reads take up to 256 sectors per command); the Wi-Fi chip's own sleep
    in standby (KSO), once the phone's standby current can be measured; the
    Adreno 330; sound (the WCD9320 behind the audio DSP).
