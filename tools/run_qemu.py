#!/usr/bin/env python3
"""Launch SOS2 in QEMU with optional writable storage: QEMU's ARM machine, its
AArch64 one with --arch arm64, or its x64 microvm with --arch x64; or,
with --machine pc, a PC (q35 with OVMF's UEFI) started from the USB stick
that make pc builds."""
import argparse
import os
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]

# Per architecture: QEMU binary (and its override variable), CPU, CPU limit,
# RAM (the arm32 page pool, 640 MiB, about the Redmi 2's, is fixed in
# arch/arm/resources.ld; arm64's spans the free stretches the DTB reports).
MACHINES = {"arm32": ("QEMU_ARM", "qemu-system-arm", "cortex-a15", 4, "2G"),
            "arm64": ("QEMU_ARM64", "qemu-system-aarch64", "cortex-a53", 8, "2G"),
            # The CPU follows the accelerator (x86_cpu).
            "x64": ("QEMU_X86_64", "qemu-system-x86_64", None, 8, "2G")}
AUDIO = ("pa", "pipewire", "alsa", "sdl")

def default_audio(display):
    """With a window, PulseAudio's server, which PipeWire also provides, else
    PipeWire's own: QEMU stops at start when its server is missing. Without a
    window or either server, no sound device."""
    if display not in ("gtk", "sdl"):
        return "none"
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
    if os.environ.get("PULSE_SERVER") or (runtime / "pulse/native").exists():
        return "pa"
    return "pipewire" if (runtime / "pipewire-0").exists() else "none"

def kvm_usable():
    return os.access("/dev/kvm", os.R_OK | os.W_OK)

def x86_cpu(accel):
    """KVM's host CPU with an invariant TSC, whose frequency QEMU then reports
    (arch/x64/apic.c); TCG's most capable model."""
    return "host,+invtsc" if accel == "kvm" else "max"

# OVMF's code and variable store, as distributions install them; OVMF_CODE
# and OVMF_VARS name others.
OVMF = (("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
        ("/usr/share/edk2/x64/OVMF_CODE.4m.fd", "/usr/share/edk2/x64/OVMF_VARS.4m.fd"),
        ("/usr/share/edk2/ovmf/OVMF_CODE.fd", "/usr/share/edk2/ovmf/OVMF_VARS.fd"),
        ("/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd"))

def ovmf():
    if os.environ.get("OVMF_CODE") and os.environ.get("OVMF_VARS"):
        return Path(os.environ["OVMF_CODE"]), Path(os.environ["OVMF_VARS"])
    for code, variables in OVMF:
        if Path(code).is_file() and Path(variables).is_file():
            return Path(code), Path(variables)
    raise ValueError("--machine pc needs OVMF (the ovmf or edk2-ovmf package), or OVMF_CODE and OVMF_VARS")

def pc_command(stick, display="gtk", cpus=4, audio=None, accel=None, pointer="tablet"):
    """A PC: q35 with OVMF, whose writable variable store sits beside the
    stick; std VGA, whose GOP framebuffer is the screen; the stick, a USB
    keyboard and a USB tablet or mouse on xHCI; an e1000e and HD Audio."""
    accel = accel or ("kvm" if kvm_usable() else "tcg")
    if not 1 <= cpus <= MACHINES["x64"][3]:
        raise ValueError(f"CPU count must be between 1 and {MACHINES['x64'][3]}")
    code, template = ovmf()
    variables = Path(stick).resolve().with_name("ovmf-vars.fd")
    if not variables.is_file():
        shutil.copyfile(template, variables)
    audio = audio or default_audio(display)
    backend, _, wav = audio.partition(":")
    if not (audio == "none" or audio in AUDIO or (backend == "wav" and wav)):
        raise ValueError(f"--audio: {', '.join(AUDIO)}, wav:FILE or none")
    def path(value):
        return str(Path(value).resolve()).replace(",", ",,")
    cmd = [os.environ.get("QEMU_X86_64", "qemu-system-x86_64"), "-name", "sos2", "-machine", "q35",
           "-cpu", x86_cpu(accel), "-accel", accel, "-smp", str(cpus), "-m", "2G", "-nodefaults",
           "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={path(code)}",
           "-drive", f"if=pflash,format=raw,unit=1,file={path(variables)}",
           "-display", display, "-serial", "none", "-monitor", "none", "-device", "VGA",
           "-device", "qemu-xhci,id=xhci",
           "-drive", f"if=none,id=stick,format=raw,file={path(stick)}",
           "-device", "usb-storage,bus=xhci.0,drive=stick,bootindex=0",
           "-device", "usb-kbd,bus=xhci.0", "-device", f"usb-{pointer},bus=xhci.0",
           "-netdev", "user,id=sos2net", "-device", "e1000e,netdev=sos2net,mac=52:54:00:53:4f:53"]
    if audio != "none":
        options = f"{backend},id=sos2audio,out.frequency=48000"
        if wav:
            options += ",path=" + path(wav)
        cmd += ["-audiodev", options, "-device", "ich9-intel-hda", "-device", "hda-output,audiodev=sos2audio"]
    return cmd

def command(display="gtk", bundle=None, disk=None, ram_only=False, cpus=4, graphics="ramfb", arch="arm32",
            jsc=None, audio=None, accel=None):
    variable, binary, cpu, limit, memory = MACHINES[arch]
    x86 = arch == "x64"
    accel = accel or ("kvm" if x86 and kvm_usable() else "tcg")
    if accel == "kvm" and not x86:
        raise ValueError("--accel kvm runs only the x64 system on this x86 host")
    if x86 and graphics == "ramfb":
        raise ValueError("--graphics: QEMU's microvm has no ramfb; use virtio-gpu or virgl")
    if not 1 <= cpus <= limit:
        raise ValueError(f'CPU count must be between 1 and {limit}')
    # As the phone checks them (tools/package_redmi2.py): the kernel takes up
    # to 127 bytes and stops at boot on more.
    if jsc is not None and (not jsc or len(jsc) > 127 or not all('!' <= c <= '~' for c in jsc)):
        raise ValueError('--jsc: 1-127 printable ASCII characters, comma-separated JavaScriptCore options')
    audio = audio or default_audio(display)
    backend, _, wav = audio.partition(":")
    if not (audio == "none" or audio in AUDIO or (backend == "wav" and wav)):
        raise ValueError(f"--audio: {', '.join(AUDIO)}, wav:FILE or none")
    if graphics == 'virgl' and display in ('gtk', 'sdl'):
        display += ',gl=on'
    if x86:
        # QEMU's microvm, booted through the kernel's PVH note: virtio-mmio
        # devices as on the ARM machines, ACPI's MADT for the CPUs, the PIT
        # and the CMOS clock; no 8259 and no serial port.
        machine = ["-machine", "microvm,acpi=on,rtc=on,pit=on,pic=off,isa-serial=off",
                   "-cpu", x86_cpu(accel), "-accel", accel]
        kernel = ROOT / 'build/resources-x64/kernel.elf'
    else:
        machine = ["-machine", "virt-10.2,secure=off,virtualization=off,gic-version=2", "-cpu", cpu, "-accel", "tcg"]
        kernel = ROOT / f'build/resources-{arch}/kernel.img'
    cmd = [os.environ.get(variable, binary),
           "-name", "sos2", *machine, "-smp", str(cpus), "-m", memory,
           "-kernel", str(kernel),
           "-display", display, "-serial", "none", "-monitor", "none",
           "-nic", "none", "-global", "virtio-mmio.force-legacy=false",
           "-netdev","user,id=sos2net","-device","virtio-net-device,netdev=sos2net,id=sos2net,mac=52:54:00:53:4f:53"]
    if graphics in ('virtio-gpu', 'virgl'):
        device = 'virtio-gpu-gl-device' if graphics == 'virgl' else 'virtio-gpu-device'
        cmd += ["-device", device+",id=sos2gpu,xres=480,yres=800,max_outputs=1"]
    if not x86:
        cmd += ["-device", "ramfb,id=sos2ramfb"]
    cmd += ["-device", "virtio-tablet-device", "-device", "virtio-keyboard-device"]
    if disk:
        escaped=str(Path(disk).resolve()).replace(",",",,")
        cmd += ["-drive",f"if=none,id=sos2disk,file={escaped},format=raw,cache=writeback",
                "-device","virtio-blk-device,drive=sos2disk"]
    if bundle:
        if arch in ('arm64', 'x64'):
            # As an Android boot image's ramdisk: the DTB (or x64's PVH
            # start information) says where it lies, and a compressed (SORZ)
            # bundle expands at boot.
            cmd += ["-initrd", str(Path(bundle).resolve())]
        else:
            escaped=str(Path(bundle).resolve()).replace(",",",,")
            cmd += ["-device",f"loader,file={escaped},addr=0x50000000,force-raw=on"]
    if audio != "none":
        # The guest's rate, so that QEMU does not resample, and one stream, an
        # output: nothing records.
        options = f"{backend},id=sos2audio,out.frequency=48000"
        if wav:
            options += ",path=" + str(Path(wav).resolve()).replace(",", ",,")
        cmd += ["-audiodev", options, "-device", "virtio-sound-device,audiodev=sos2audio,streams=1"]
    arguments = (["sos2.storage=ram"] if ram_only else []) + ([f"sos2.jsc={jsc}"] if jsc else [])
    cmd += ["-device", "virtio-rng-device", "-append", " ".join(arguments)]
    return cmd

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=tuple(MACHINES), default=os.environ.get("SOS2_ARCH", "arm32"),
                        help="QEMU machine and kernel (default: SOS2_ARCH or arm32)")
    parser.add_argument("--machine", choices=("qemu", "pc"), default="qemu",
                        help="pc: a PC (q35 with OVMF) started from --stick, x64 only")
    parser.add_argument("--stick", type=Path, help="the USB stick image make pc builds, for --machine pc")
    parser.add_argument("--pointer", choices=("tablet", "mouse"), default="tablet",
                        help="--machine pc: a USB tablet (absolute, follows the window's pointer) or "
                             "a USB mouse (relative, as a PC's)")
    parser.add_argument("--ram-only",action="store_true",help="temporary session; ignore disks and keep website data in RAM")
    parser.add_argument("--cpus",type=int,choices=range(1,9),default=4,metavar="N",
                        help="CPU cores: 1-4, or 1-8 for arm64 and x64 (default: 4)")
    parser.add_argument("--graphics",choices=('ramfb','virtio-gpu','virgl'),default=None,
                        help='RAM framebuffer, 2D transport or GPU composition; drawing uses Skia on the CPU '
                             '(default: ramfb, or virtio-gpu for x64, whose microvm has no ramfb)')
    parser.add_argument("--accel",choices=('kvm','tcg'),
                        help='x64 only: KVM (the default where /dev/kvm is usable) or TCG emulation')
    parser.add_argument("--jsc", metavar="OPTIONS",
                        help="JavaScriptCore options for pages, comma-separated: useJIT=false runs the "
                             "interpreter, useDFGJIT=false stops at the baseline JIT")
    parser.add_argument("--display", default="gtk", choices=("gtk", "sdl"))
    parser.add_argument("--audio", metavar="OUTPUT",
                        help="sound through pa (PulseAudio, or PipeWire's PulseAudio server), pipewire, alsa "
                             "or sdl; wav:FILE records it; none has no sound device, so pages play silently "
                             "as on the phone and the tablet (default: pa or pipewire when one runs, else none)")
    parser.add_argument("--bundle", type=Path, help="read-only resource image: loaded at 0x50000000 on arm32, the "
                                                    "initrd on arm64 and x64 (raw, or SORZ-compressed)")
    parser.add_argument("--disk", type=Path, help="existing writable disk image; make run STORAGE=persistent creates one safely")
    args = parser.parse_args()
    if args.bundle and not args.bundle.is_file():
        parser.error("--bundle must be an existing resource image")
    if args.disk and not args.disk.is_file():
        parser.error("--disk must be an existing disk image; the launcher never formats disks")
    if args.machine == "pc":
        if args.arch != "x64":
            parser.error("--machine pc runs the x64 system: use --arch x64")
        if not args.stick or not args.stick.is_file():
            parser.error("--machine pc needs --stick, the image make pc builds")
        if args.bundle or args.disk or args.ram_only or args.jsc or args.graphics:
            parser.error("--machine pc starts the stick's own system: --bundle, --disk, --ram-only, --jsc "
                         "and --graphics belong to make pc and the QEMU machines")
        try:
            cmd = pc_command(args.stick, display=args.display, cpus=args.cpus, audio=args.audio,
                             accel=args.accel, pointer=args.pointer)
        except ValueError as error:
            parser.error(str(error))
        if not shutil.which(cmd[0]):
            parser.error(f"{cmd[0]} is missing; install qemu-system-x86_64")
        print("Starting sos2 on a PC in QEMU. Close its window or press Ctrl-C to stop.", flush=True)
        os.execvp(cmd[0], cmd)
    graphics = args.graphics or ('virtio-gpu' if args.arch == 'x64' else 'ramfb')
    try:
        cmd = command(display=args.display, graphics=graphics, bundle=args.bundle, disk=args.disk,
                      ram_only=args.ram_only, cpus=args.cpus, arch=args.arch, jsc=args.jsc, audio=args.audio,
                      accel=args.accel)
    except ValueError as error:
        parser.error(str(error))
    if not shutil.which(cmd[0]):
        parser.error(f"{cmd[0]} is missing; install {MACHINES[args.arch][1]}")
    if not Path(cmd[cmd.index("-kernel")+1]).is_file():
        parser.error("kernel image is missing; run make all" + ("" if args.arch == "arm32" else " ARCH=" + args.arch))
    print("Starting sos2 in QEMU. Close its window or press Ctrl-C to stop.", flush=True)
    os.execvp(cmd[0], cmd)

if __name__ == "__main__":
    main()
