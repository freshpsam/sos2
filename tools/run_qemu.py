#!/usr/bin/env python3
"""Launch the native ARM32 browser with optional writable storage."""
import argparse
import os
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]

def command(image=None, display="gtk", input_device=True, ramfb=True,
            bundle=None, disk=None, keyboard=True, network=False,ram_only=False,cpus=4,graphics="ramfb"):
    if cpus not in (1,2,3,4):
        raise ValueError('CPU count must be between 1 and 4')
    if graphics not in ('ramfb', 'virtio-gpu', 'virgl'):
        raise ValueError('unsupported graphics backend')
    if graphics == 'virgl':
        if display in ('gtk', 'sdl'):
            display += ',gl=on'
        elif display != 'egl-headless':
            raise ValueError('virgl needs an OpenGL display: gtk, sdl or egl-headless')
    default_image = ROOT / 'build/resources-arm32/kernel.img'
    cmd = [os.environ.get("QEMU_ARM", "qemu-system-arm"),
           "-name", "sos2",
           "-machine", "virt-10.2,secure=off,virtualization=off,gic-version=2",
           "-cpu", "cortex-a15", "-accel", "tcg", "-smp", str(cpus), "-m", "1G",
           "-kernel", str(image or default_image),
           "-display", display, "-serial", "none", "-monitor", "none",
           "-nic", "none", "-global", "virtio-mmio.force-legacy=false"]
    if network:
        cmd += ["-netdev","user,id=sos2net","-device","virtio-net-device,netdev=sos2net,id=sos2net,mac=52:54:00:53:4f:53"]
    if ramfb:
        if graphics in ('virtio-gpu', 'virgl'):
            device = 'virtio-gpu-gl-device' if graphics == 'virgl' else 'virtio-gpu-device'
            cmd += ["-device", device+",id=sos2gpu,xres=480,yres=800,max_outputs=1"]
        cmd += ["-device", "ramfb,id=sos2ramfb"]
    if input_device:
        cmd += ["-device", "virtio-tablet-device"]
        if keyboard:
            cmd += ["-device", "virtio-keyboard-device"]
    if disk:
        escaped=str(Path(disk).resolve()).replace(",",",,")
        cmd += ["-drive",f"if=none,id=sos2disk,file={escaped},format=raw,cache=writeback",
                "-device","virtio-blk-device,drive=sos2disk"]
    if bundle:
        escaped=str(Path(bundle).resolve()).replace(",",",,")
        cmd += ["-device",f"loader,file={escaped},addr=0x50000000,force-raw=on"]
    cmd += ["-device", "virtio-rng-device", "-append", "sos2.storage=ram" if ram_only else ""]
    return cmd

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-keyboard",action="store_true",help="use the on-screen keyboard without a virtual hardware keyboard")
    parser.add_argument("--network",action="store_true",help="enable outbound user-mode Ethernet for native ARM32")
    parser.add_argument("--ram-only",action="store_true",help="temporary session; ignore disks and keep website data in RAM")
    parser.add_argument("--cpus",type=int,choices=(1,2,3,4),default=4,help="CPU cores (default: 4)")
    parser.add_argument("--graphics",choices=('ramfb','virtio-gpu','virgl'),default='ramfb',
                        help='RAM framebuffer, 2D transport or GPU composition; drawing uses Skia on the CPU')
    parser.add_argument("--display", default="gtk", choices=("gtk", "sdl", "none", "egl-headless"))
    parser.add_argument("--image", type=Path, help="kernel image to boot")
    parser.add_argument("--bundle", type=Path, help="read-only resource image; selects the ARM32 resource kernel with 1 GiB RAM")
    parser.add_argument("--disk", type=Path, help="existing writable disk image; make run STORAGE=persistent creates one safely")
    args = parser.parse_args()
    if args.bundle and not args.bundle.is_file():
        parser.error("--bundle must be an existing resource image")
    if args.disk and not args.disk.is_file():
        parser.error("--disk must be an existing disk image; the launcher never formats disks")
    try:
        cmd = command(image=args.image,display=args.display,graphics=args.graphics,
                      bundle=args.bundle,disk=args.disk,keyboard=not args.no_keyboard,network=args.network,ram_only=args.ram_only,cpus=args.cpus)
    except ValueError as error:
        parser.error(str(error))
    if not shutil.which(cmd[0]):
        parser.error(f"{cmd[0]} is missing; install qemu-system-arm")
    if not Path(cmd[cmd.index("-kernel")+1]).is_file():
        parser.error("kernel image is missing; run make all")
    print("Starting sos2 in QEMU. Close its window or press Ctrl-C to stop.", flush=True)
    os.execvp(cmd[0], cmd)

if __name__ == "__main__":
    main()
