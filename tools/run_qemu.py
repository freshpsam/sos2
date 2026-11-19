#!/usr/bin/env python3
"""Launch SOS2 in QEMU with optional writable storage: QEMU's ARM machine, or
its AArch64 one with --arch arm64."""
import argparse
import os
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]

# Per architecture: QEMU binary (and its override variable), CPU, CPU limit,
# RAM (the arm32 page pool is fixed in arch/arm/resources.ld; arm64 takes the
# largest free stretch the DTB reports).
MACHINES = {"arm32": ("QEMU_ARM", "qemu-system-arm", "cortex-a15", 4, "1G"),
            "arm64": ("QEMU_ARM64", "qemu-system-aarch64", "cortex-a53", 8, "2G")}

def command(display="gtk", bundle=None, disk=None, ram_only=False, cpus=4, graphics="ramfb", arch="arm32",
            jsc=None):
    variable, binary, cpu, limit, memory = MACHINES[arch]
    if not 1 <= cpus <= limit:
        raise ValueError(f'CPU count must be between 1 and {limit}')
    # As the phone checks them (tools/package_redmi2.py): the kernel takes up
    # to 127 bytes and stops at boot on more.
    if jsc is not None and (not jsc or len(jsc) > 127 or not all('!' <= c <= '~' for c in jsc)):
        raise ValueError('--jsc: 1-127 printable ASCII characters, comma-separated JavaScriptCore options')
    if graphics == 'virgl' and display in ('gtk', 'sdl'):
        display += ',gl=on'
    cmd = [os.environ.get(variable, binary),
           "-name", "sos2",
           "-machine", "virt-10.2,secure=off,virtualization=off,gic-version=2",
           "-cpu", cpu, "-accel", "tcg", "-smp", str(cpus), "-m", memory,
           "-kernel", str(ROOT / f'build/resources-{arch}/kernel.img'),
           "-display", display, "-serial", "none", "-monitor", "none",
           "-nic", "none", "-global", "virtio-mmio.force-legacy=false",
           "-netdev","user,id=sos2net","-device","virtio-net-device,netdev=sos2net,id=sos2net,mac=52:54:00:53:4f:53"]
    if graphics in ('virtio-gpu', 'virgl'):
        device = 'virtio-gpu-gl-device' if graphics == 'virgl' else 'virtio-gpu-device'
        cmd += ["-device", device+",id=sos2gpu,xres=480,yres=800,max_outputs=1"]
    cmd += ["-device", "ramfb,id=sos2ramfb", "-device", "virtio-tablet-device",
            "-device", "virtio-keyboard-device"]
    if disk:
        escaped=str(Path(disk).resolve()).replace(",",",,")
        cmd += ["-drive",f"if=none,id=sos2disk,file={escaped},format=raw,cache=writeback",
                "-device","virtio-blk-device,drive=sos2disk"]
    if bundle:
        if arch == 'arm64':
            # As an Android boot image's ramdisk: the DTB says where it lies,
            # and a compressed (SORZ) bundle expands at boot.
            cmd += ["-initrd", str(Path(bundle).resolve())]
        else:
            escaped=str(Path(bundle).resolve()).replace(",",",,")
            cmd += ["-device",f"loader,file={escaped},addr=0x50000000,force-raw=on"]
    arguments = (["sos2.storage=ram"] if ram_only else []) + ([f"sos2.jsc={jsc}"] if jsc else [])
    cmd += ["-device", "virtio-rng-device", "-append", " ".join(arguments)]
    return cmd

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=tuple(MACHINES), default=os.environ.get("SOS2_ARCH", "arm32"),
                        help="QEMU machine and kernel (default: SOS2_ARCH or arm32)")
    parser.add_argument("--ram-only",action="store_true",help="temporary session; ignore disks and keep website data in RAM")
    parser.add_argument("--cpus",type=int,choices=range(1,9),default=4,metavar="N",
                        help="CPU cores: 1-4, or 1-8 for arm64 (default: 4)")
    parser.add_argument("--graphics",choices=('ramfb','virtio-gpu','virgl'),default='ramfb',
                        help='RAM framebuffer, 2D transport or GPU composition; drawing uses Skia on the CPU')
    parser.add_argument("--jsc", metavar="OPTIONS",
                        help="JavaScriptCore options for pages, comma-separated: useJIT=false runs the "
                             "interpreter, useDFGJIT=false stops at the baseline JIT")
    parser.add_argument("--display", default="gtk", choices=("gtk", "sdl"))
    parser.add_argument("--bundle", type=Path, help="read-only resource image: loaded at 0x50000000 on arm32, the "
                                                    "initrd on arm64 (raw, or SORZ-compressed)")
    parser.add_argument("--disk", type=Path, help="existing writable disk image; make run STORAGE=persistent creates one safely")
    args = parser.parse_args()
    if args.bundle and not args.bundle.is_file():
        parser.error("--bundle must be an existing resource image")
    if args.disk and not args.disk.is_file():
        parser.error("--disk must be an existing disk image; the launcher never formats disks")
    try:
        cmd = command(display=args.display, graphics=args.graphics, bundle=args.bundle, disk=args.disk,
                      ram_only=args.ram_only, cpus=args.cpus, arch=args.arch, jsc=args.jsc)
    except ValueError as error:
        parser.error(str(error))
    if not shutil.which(cmd[0]):
        parser.error(f"{cmd[0]} is missing; install {MACHINES[args.arch][1]}")
    if not Path(cmd[cmd.index("-kernel")+1]).is_file():
        parser.error("kernel image is missing; run make all" + (" ARCH=arm64" if args.arch == "arm64" else ""))
    print("Starting sos2 in QEMU. Close its window or press Ctrl-C to stop.", flush=True)
    os.execvp(cmd[0], cmd)

if __name__ == "__main__":
    main()
