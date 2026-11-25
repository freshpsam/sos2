#!/usr/bin/env python3
"""Build the Nexus 5's (LG hammerhead) RAM boot image; never access a device.

The stock bootloader (HHZ20h, unlocked) starts what `fastboot boot` sends it:
an Android boot v0 image of 2 KiB pages with the ARM32 kernel, a raw image
with a zImage-shaped header whose end word points at the device tree appended
to it (boards/nexus5/boot.dts, which the bootloader picks by qcom,msm-id), and
the resources, engine included, as a SORZ ramdisk (tools/package_redmi2.py's
format), which the kernel expands at boot. The header has the bootloader put
the kernel at 0x8000, its copy of the device tree at 0x02700000 and the
ramdisk at 0x02900000, as for hammerhead's own images (the ramdisk ends below
the firmware's memory hole at 0x07B00000, which holds the splash). The
installed system (include/system_store.h) is the same image with
sos2.storage=installed, which the 22 MiB boot partition holds whole and which
keeps its app store and data volume in the cache partition
(boards/nexus5/emmc.c). As on the Redmi 2, the images build in no apps: the
system's updater installs them, and --apps-dir writes the bundle's apps as
ARM32 packages with their index.txt, the Redmi 2's too. With
--wifi-firmware, the image carries the Wi-Fi chip's firmware and its board's
calibration from the stock system partition (boards/nexus5/wifi.c); with
--wifi-profile too, a profile made by tools/redmi2_wifi.py, which the phone
joins at boot (the image then holds the network's key: keep it private).
Writes boot.img and boot.img.json to --output-dir, and with --install-dir
what tools/publish_pages.py publishes there: flash-boot.img, sos2-nexus5.update
(the updater's file) and install.json. See docs/nexus5.md.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import package_redmi2 as resources
from package_redmi2 import PackagingError
from redmi2_wifi import WifiError, profile_bytes

KERNEL_ADDRESS = 0x00008000
TAGS_ADDRESS = 0x02700000
RAMDISK_ADDRESS = 0x02900000  # BOOT_RAMDISK_BASE in include/boot_bundle.h
RAMDISK_LIMIT = 0x07B00000    # BOOT_RAMDISK_END: the firmware's memory hole
SECOND_ADDRESS = 0x00F00000
PAGE_SIZE = 2048
BOOT_PARTITION_SIZE = 22 * 1024 * 1024  # hammerhead's: SYSTEM_BOOT_LIMIT in include/system_store.h
MSM_ID = struct.pack('>3I', 126, 150, 0x20002)
CMDLINE = b'sos2.storage=ram'
INSTALLED_CMDLINE = b'sos2.storage=installed'
BOARD = b'lge-hammerhead'  # the kernel's (kernel/protection/system_update.c)
INSTALL_FILES = ('flash-boot.img', 'sos2-nexus5.update')
# The BCM4339's firmware and its board's calibration (NVRAM text), as
# Android 6's /system/vendor/firmware/fw_bcmdhd.bin and
# /system/etc/wifi/bcmdhd.cal.
WIFI_FIRMWARE = ('fw_bcmdhd.bin', 'bcmdhd.cal')


def check_dtb(dtb):
    """One FDT v17 blob (tools/package_redmi2.py's parse_dtb) whose root names
    hammerhead and its qcom,msm-id, by which the bootloader picks it."""
    root = resources.parse_dtb(dtb)['/']
    if b'lge,hammerhead' not in root.get('compatible', b'').split(b'\0') or root.get('qcom,msm-id') != MSM_ID:
        raise PackagingError("--dtb lacks hammerhead's compatible or qcom,msm-id")


def wifi_firmware(folder):
    """/firmware/brcm's files from a folder of the stock system partition's,
    each checked: the firmware names the BCM4339 (4339a0) and an SDIO build,
    and the calibration is text naming the board (boardtype=0x06b6 on
    hammerhead)."""
    files = {}
    for name in WIFI_FIRMWARE:
        path = folder / name
        data = path.read_bytes()
        if name.endswith('.bin'):
            if not 64 * 1024 <= len(data) <= 1024 * 1024 or b'4339a0' not in data or b'/sdio-' not in data:
                raise PackagingError(f'{path}: not a BCM4339 SDIO firmware file')
        elif len(data) > 16 * 1024 or b'boardtype=' not in data or any(b > 0x7e and b != 0 for b in data):
            raise PackagingError(f'{path}: not a Broadcom NVRAM text file')
        files[b'/firmware/brcm/' + name.encode()] = data
    return files


def boot_image(payload, cmdline, ramdisk):
    """An Android boot v0 image (tools/package_redmi2.py's android_image) with
    hammerhead's addresses; the boot partition must hold it."""
    image = resources.android_image(payload, cmdline, ramdisk, KERNEL_ADDRESS, RAMDISK_ADDRESS,
                                    SECOND_ADDRESS, TAGS_ADDRESS, b'sos2-nexus5')
    if len(image) > BOOT_PARTITION_SIZE:
        raise PackagingError(f'the {len(image)}-byte boot image exceeds the {BOOT_PARTITION_SIZE}-byte boot partition')
    return image


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kernel', type=Path, required=True, help='build/nexus5-arm32/kernel.img')
    parser.add_argument('--elf', type=Path, required=True, help='build/nexus5-arm32/kernel.elf')
    parser.add_argument('--dtb', type=Path, required=True, help='build/nexus5/boot.dtb')
    parser.add_argument('--resources', type=Path, required=True, help='the ARM32 native-web.bundle')
    parser.add_argument('--release', required=True, help="the build's name, shown by the updater")
    parser.add_argument('--jsc', help='JavaScriptCore options for pages, comma-separated (the sos2.jsc= boot argument)')
    parser.add_argument('--wifi-firmware', type=Path,
                        help='folder with the stock fw_bcmdhd.bin and bcmdhd.cal; without it, no Wi-Fi')
    parser.add_argument('--wifi-profile', type=Path,
                        help='a Wi-Fi profile made by tools/redmi2_wifi.py, joined at boot (open or WPA2)')
    parser.add_argument('--install-dir', type=Path,
                        help='write the installed system, its update and install.json here')
    parser.add_argument('--apps-dir', type=Path, help='write the apps as packages, with index.txt, here')
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        kernel, elf, dtb = args.kernel.read_bytes(), args.elf.read_bytes(), args.dtb.read_bytes()
        allocation = resources.validate_arm32_kernel(kernel, elf, KERNEL_ADDRESS, TAGS_ADDRESS, 'Nexus 5')
        check_dtb(dtb)
        # The bootloader copies the kernel and its appended DTB, in whole
        # pages, into the kernel's allocation.
        if resources.align(len(kernel) + len(dtb), PAGE_SIZE) > allocation:
            raise PackagingError("the kernel's allocation must hold the bootloader's copy of it and its DTB")
        bundle = args.resources.read_bytes()
        expanded = resources.resource_bundle(bundle, ram_only=True, release=args.release, strip_apps=True)
        if args.wifi_profile and not args.wifi_firmware:
            raise PackagingError('a Wi-Fi profile needs the Wi-Fi firmware')
        if args.wifi_firmware:
            files = resources.bundle_files(expanded)
            if any(name.startswith(b'/firmware/') for name in files) or b'/system/wifi-profile' in files:
                raise PackagingError('input bundle already contains firmware or a Wi-Fi profile')
            files.update(wifi_firmware(args.wifi_firmware))
            if args.wifi_profile:
                try:
                    config = json.loads(args.wifi_profile.read_bytes())
                    if isinstance(config, dict) and config.get('security') == 'wpa3-sae':
                        raise WifiError("the Nexus 5's Wi-Fi firmware has no WPA3")
                    files[b'/system/wifi-profile'] = profile_bytes(config)
                except (ValueError, WifiError) as error:
                    raise PackagingError(f'{args.wifi_profile}: {error}') from error
            expanded = resources.sorb(files)
        ramdisk = resources.sorz(expanded)
        if RAMDISK_ADDRESS + resources.align(len(ramdisk), PAGE_SIZE) > RAMDISK_LIMIT:
            raise PackagingError(f'the {len(ramdisk)}-byte ramdisk does not fit between 0x{RAMDISK_ADDRESS:08x} '
                                 f'and the firmware at 0x{RAMDISK_LIMIT:08x}')
        cmdline = resources.with_jsc_options(CMDLINE, args.jsc)
        image = boot_image(kernel + dtb, cmdline, ramdisk)
        # The installed system shares the RAM image's resources, policy included.
        installed_cmdline = resources.with_jsc_options(INSTALLED_CMDLINE, args.jsc)
        installed = boot_image(kernel + dtb, installed_cmdline, ramdisk)
        manifest = {
            'target': 'Nexus 5 (LG hammerhead, MSM8974), stock bootloader HHZ20h',
            'format': 'Android boot v0; ARM32 zImage-shaped kernel followed by one device tree',
            'release': args.release,
            'kernel_address': f'0x{KERNEL_ADDRESS:08x}',
            'kernel_allocation_bytes': allocation,
            'dtb_address': f'0x{TAGS_ADDRESS:08x}',
            'ramdisk_address': f'0x{RAMDISK_ADDRESS:08x}',
            'ramdisk_bytes': len(ramdisk),
            'expanded_bytes': len(expanded),
            'boot_image_bytes': len(image),
            'cmdline': cmdline.decode('ascii'),
            'apps_left_out': resources.app_ids(resources.bundle_files(bundle)),
            'wifi_firmware': bool(args.wifi_firmware),
            'wifi_profile': bool(args.wifi_profile),
            'sha256': {name: hashlib.sha256(value).hexdigest()
                       for name, value in (('kernel', kernel), ('dtb', dtb), ('boot_image', image))},
            'boot': 'fastboot boot boot.img',
        }
        outputs = [(args.output_dir / 'boot.img', image),
                   (args.output_dir / 'boot.img.json', json.dumps(manifest, indent=2).encode() + b'\n')]
        if args.install_dir:
            # The updater's file is the image after its 4 KiB header
            # (kernel/protection/system_update.c).
            outputs += [(args.install_dir / name, data) for name, data in
                        resources.install_outputs(args.release, BOARD, INSTALL_FILES, installed,
                                                  bool(args.wifi_profile), installed_cmdline,
                                                  wifi_firmware=bool(args.wifi_firmware)).items()]
        packages = {}
        if args.apps_dir:
            packages = resources.app_packages(resources.bundle_files(bundle))
            outputs += [(args.apps_dir / f'{app_id}.sos2app', package) for app_id, package in packages.items()]
            outputs.append((args.apps_dir / 'index.txt', resources.app_index(packages)))
        inputs = {path.resolve() for path in (args.kernel, args.elf, args.dtb, args.resources)}
        if args.wifi_profile:
            inputs.add(args.wifi_profile.resolve())
        if any(path.resolve() in inputs or (args.wifi_firmware and path.resolve().is_relative_to(
                args.wifi_firmware.resolve())) for path, _ in outputs):
            raise PackagingError('an output would overwrite an input file')
        for directory in {path.parent for path, _ in outputs}:
            directory.mkdir(parents=True, exist_ok=True)
        resources.write_outputs(outputs)
        if args.apps_dir:
            # Packages of apps no longer in the bundle are not this build's.
            for stale in args.apps_dir.glob('*.sos2app'):
                if stale.stem not in packages:
                    stale.unlink()
    except (OSError, PackagingError) as error:
        parser.exit(1, f'{error}\n')
    print(f"{args.output_dir / 'boot.img'}: {len(image)} bytes, sha256 {manifest['sha256']['boot_image']}")
    if args.install_dir:
        print(f"{args.install_dir / 'flash-boot.img'}: {len(installed)} bytes, and its update and install.json")
    if not args.wifi_firmware:
        print('No Wi-Fi firmware: the Nexus 5 has no Wi-Fi without NEXUS5_STOCK (docs/nexus5.md)')


if __name__ == '__main__':
    sys.exit(main())
