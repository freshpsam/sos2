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
   installed systems' eMMC data. Multi-block transfers would shorten the
   phone's boot-time read.
4. **Compositing and GPU drawing.** Composited fixed and overflow-scrolling
   elements and masks in the CPU compositor; damage tracking and worker-thread
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
   media and other web APIs; website-data controls and per-site process
   isolation, since every page shares the browser profile.
8. **Power.** Standby draws 6.0 mA. Next: Wi-Fi's share in power save, the
   display's power domain, then clock and voltage scaling, sleep with the
   screen on, and thermal handling.
