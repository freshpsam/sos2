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
3. **Persistent phone storage.** Keep the installed phone's settings and
   profiles on its eMMC, and validate power loss and durability. Multi-block
   transfers would shorten the boot-time read.
4. **Compositing and GPU drawing.** Composited fixed and overflow-scrolling
   elements and masks in the CPU compositor; damage tracking and worker-thread
   tiles in the GPU compositor; fewer device requests per frame with virgl;
   qualify the [Adreno path](gpu.md#redmi-2-adreno), then bring GPU drawing to
   the Adreno 306 through freedreno.
5. **AArch64 and JavaScript JIT.** WebKit has no ARMv7 JIT, so pages run the
   interpreter. Port the kernel, SDK and WebKit to AArch64 and compare the JIT
   tiers with the interpreter.
6. **Updates and time.** Roll back to the previous slot when a release fails
   to start; authenticated time and time zones.
7. **Web platform.** IndexedDB, service workers, Cache Storage, downloads,
   media and other web APIs; website-data controls and per-site process
   isolation, since every page shares the browser profile.
8. **Power.** Standby draws 6.0 mA. Next: Wi-Fi's share in power save, the
   display's power domain, then clock and voltage scaling, sleep with the
   screen on, and thermal handling.
