#!/usr/bin/env python3
"""Build the Galaxy Tab A7 Lite's (SM-T220) boot image; never access a device.

LK, the tablet's bootloader, starts the boot partition's Android image: header
v2 with 2 KiB pages, the arm64 kernel gzip-compressed, the resources, engine
included, as a SORZ ramdisk (tools/package_redmi2.py's format), which the
kernel expands at boot, and the stock image's device trees (an Android DT
table), of which LK picks one and completes it before handing it over. The
stock boot.img and vbmeta.img come from the tablet's firmware, given with
--stock. With --touch-firmware, the ramdisk also carries the touch
controller's firmware files from the stock vendor partition, which the kernel
sends to the controller at boot (boards/gta7lite/touch.c), and with
--wifi-firmware the connectivity subsystem's and Wi-Fi's firmware from the
same partition (boards/gta7lite/connsys.c, wifi_radio.c), and with
--wifi-profile, a profile made by tools/redmi2_wifi.py, which the tablet joins
at boot; it carries the network's key, so such an image stays private.
The installed system (include/system_store.h) is the same image with
sos2.storage=installed, which keeps its app store and data volume in the cache
partition (boards/gta7lite/emmc.c). As on the phone, the images build in no
apps: the system's updater installs them, and --apps-dir writes the bundle's
apps as AArch64 packages with their index.txt. --install-dir writes what
tools/publish_pages.py publishes: flash-boot.img, sos2-gta7lite.update (the
updater's file) and install.json.
Writes to --output-dir:
  Image.gz, resources.sorz   always
  boot.img, boot.tar         with --stock: the RAM image, and odin4's package
                             of it
  vbmeta-disabled.tar        with --stock: the stock vbmeta with verification
                             disabled, which an unlocked tablet's LK needs
                             before it starts any but the stock boot image
  flash-boot.img             with --stock: the installed system's boot image
See docs/gta7lite.md.
"""

import argparse
import fcntl
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import sys
import tarfile

import package_redmi2 as resources
from package_redmi2 import PackagingError

# The kernel's link address, RAM + text_offset, and the start of Samsung's
# debug log region, which LK keeps: the kernel image ends below it.
KERNEL_ADDRESS = 0x40080000
TEXT_OFFSET = 0x80000
DEBUG_LOGS = 0x46C00000
PAGE_SIZE = 2048
BOOT_PARTITION_SIZE = 32 * 1024 * 1024
MAX_DTB_SIZE = 2 * 1024 * 1024  # fdt_discover's limit
HEADER_V2 = '<8s10I16s512s32s1024sIQIIQ'
HEADER_SIZE = struct.calcsize(HEADER_V2)  # 1660
# LK starts a 64-bit kernel only with this switch in the command line.
BOOTOPT = b'bootopt=64S3,32N2,64N2'
# Samsung's trailer after the image, without which LK warns at boot.
SEANDROID = b'SEANDROIDENFORCE'
AVB_VERIFICATION_DISABLED = 2
FDT_MAGIC = b'\xd0\x0d\xfe\xed'
DT_TABLE_MAGIC = 0xd7b7ab1e
FLASH_FILES = ('boot.img', 'boot.tar', 'vbmeta-disabled.tar', 'flash-boot.img')
# The NT36523 firmware files Samsung's driver picks from, by panel; the kernel
# has the same table (boards/gta7lite/touch.c).
TOUCH_FIRMWARE = ('novatek_ts_pid721f_fw.bin', 'novatek_ts_pid7227_fw.bin', 'novatek_ts_pid7221_fw.bin',
                  'novatek_ts_ntfpc_pid721f_fw.bin', 'novatek_ts_pid724a_fw.bin')
FLASH_MANIFEST = '.sos2-gta7lite-images.json'
BOARD = b'gta7litewifi'  # the kernel's (kernel/protection/system_update.c)
INSTALL_FILES = ('flash-boot.img', 'sos2-gta7lite.update')


def align(value):
    return (value + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)


def kernel_image(kernel):
    """The arm64 Image, checked against LK's load address, gzip-compressed."""
    if len(kernel) < 64 or kernel[56:60] != b'ARM\x64':
        raise PackagingError('kernel is not an arm64 Image')
    text_offset, image_size = struct.unpack_from('<2Q', kernel, 8)
    if text_offset != TEXT_OFFSET or not len(kernel) <= image_size <= DEBUG_LOGS - KERNEL_ADDRESS:
        raise PackagingError(f'the kernel must load at 0x{KERNEL_ADDRESS:08x} and end below Samsung\'s '
                             f'debug logs at 0x{DEBUG_LOGS:08x}')
    return gzip.compress(kernel, 9, mtime=0)


# The connectivity subsystem's MCU, Wi-Fi and Bluetooth RAM code and its ROM
# patch (connsys.c), then Wi-Fi's firmware (wifi_radio.c): the stock vendor
# partition's /vendor/firmware files for soc1_0 (MT6765 with the MT6631).
WIFI_FIRMWARE = ('soc1_0_ram_mcu_1_1_hdr.bin', 'soc1_0_ram_wifi_1_1_hdr.bin', 'soc1_0_ram_bt_1_1_hdr.bin',
                 'soc1_0_patch_mcu_1_1_hdr.bin', 'WIFI_RAM_CODE_soc1_0_1_1.bin')


def wifi_firmware(folder):
    """/firmware/connsys's files from a folder of the stock vendor partition's
    /vendor/firmware, each checked as the kernel reads it: the RAM code and
    patch headers name ALPS and the chip's version 0x8A; Wi-Fi's firmware
    ends with a tailer listing 1 to 10 regions."""
    files = {}
    for name in WIFI_FIRMWARE:
        path = folder / name
        data = path.read_bytes()
        if name.startswith('soc1_0_'):
            if len(data) <= 48 or data[16:20] != b'ALPS' or data[22] != 0x8a:
                raise PackagingError(f'{path}: not a soc1_0 RAM code or patch file')
        elif len(data) < 36 + 40 or not 1 <= data[-36 + 2] <= 10 or len(data) < 36 + 40 * data[-36 + 2]:
            raise PackagingError(f'{path}: not a soc1_0 Wi-Fi firmware file')
        files[b'/firmware/connsys/' + name.encode()] = data
    return files


def touch_firmware(folder):
    """/firmware/novatek's files from a folder of the stock vendor partition's
    /vendor/firmware/novatek_ts_*_fw.bin, each checked as Samsung's driver
    checks it before sending it: an "NVT" or "MOD" end flag closing a 4 KiB
    sector, and the version byte there plus its complement making 0xFF."""
    files = {}
    for name in TOUCH_FIRMWARE:
        path = folder / name
        if not path.exists():
            continue
        data = path.read_bytes()
        if not 4096 <= len(data) <= 1024 * 1024:
            raise PackagingError(f'{path}: not a touch firmware file')
        need = next((i * 4096 for i in range(len(data) // 4096, 0, -1)
                     if data[i * 4096 - 3:i * 4096] in (b'NVT', b'MOD')), 0)
        if not need or (data[need - 4096] + data[need - 4095]) & 0xff != 0xff:
            raise PackagingError(f'{path}: not a Novatek touch firmware file')
        files[b'/firmware/novatek/' + name.encode()] = data
    if not files:
        raise PackagingError(f'{folder}: none of {", ".join(TOUCH_FIRMWARE)}')
    return files


def tablet_resources(bundle, release, touch=None, wifi=None, profile=None):
    """The SORZ resources and the apps they leave out, as packages by id: the
    arm64 bundle, checked, without its apps, with the release's name, the
    touch and Wi-Fi firmware files when given, and a Wi-Fi profile's
    /system/wifi-profile; about 24 MB with the engine, which the boot
    partition's 32 MiB hold."""
    files = resources.bundle_files(bundle)
    updater = files.get(b'/bin/sos2-updater', b'')
    if len(updater) < 64 or updater[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<H', updater, 18)[0] != 183:
        raise PackagingError('the tablet needs the AArch64 system updater, /bin/sos2-updater; run make ARCH=arm64 engine')
    packages = resources.app_packages(files, elf=resources.ELF_ARM64)
    for name in [name for name in files if name.startswith(b'/apps/')]:
        del files[name]
    program = files.get(b'/bin/sos2-web', b'')
    if (len(program) < 64 or program[:7] != b'\x7fELF\x02\x01\x01' or
            struct.unpack_from('<HH', program, 16) != (2, 183)):
        raise PackagingError('the tablet needs the AArch64 /bin/sos2-web; run make ARCH=arm64 engine')
    phoff, = struct.unpack_from('<Q', program, 32)
    entry_size, entries = struct.unpack_from('<HH', program, 54)
    if entry_size != 56 or not entries or phoff + entries * entry_size > len(program) or any(
            struct.unpack_from('<I', program, phoff + i * entry_size)[0] in (2, 3) for i in range(entries)):
        raise PackagingError('browser must be static native AArch64, without a dynamic loader')
    if b'/system/release' in files:
        raise PackagingError('input bundle already names a release')
    if b'/system/storage-mode' in files:
        raise PackagingError('input bundle already contains a storage policy')
    files[b'/system/release'] = resources.validate_release(release)
    files[b'/system/storage-mode'] = b'RAM1'
    if touch or wifi:
        if any(name.startswith(b'/firmware/') for name in files):
            raise PackagingError('input bundle already contains firmware')
        files.update(touch or {})
        files.update(wifi or {})
    if profile is not None:
        if not wifi:
            raise PackagingError('a Wi-Fi profile needs the Wi-Fi firmware')
        if b'/system/wifi-profile' in files:
            raise PackagingError('input bundle already contains a Wi-Fi profile')
        files[b'/system/wifi-profile'] = profile
    data = resources.sorb(files)
    if len(data) > 256 * 1024 * 1024:
        raise PackagingError('expanded native resources exceed the kernel limit')
    return resources.sorz(data), packages


def install_files(release, image, profile):
    """The installed system for publishing (tools/publish_pages.py): its boot
    image, and the updater's file, the same image after its 4 KiB header,
    padded with zeros to whole 2 KiB pages as the kernel's header check wants
    (kernel/protection/system_update.c); and install.json with their sizes
    and SHA-256s and the commands of a first install."""
    padded = image + bytes(-len(image) % PAGE_SIZE)
    outputs = dict(zip(INSTALL_FILES, (image, resources.update_header(release, padded, BOARD) + padded)))
    manifest = {
        'release': release,
        'board': BOARD.decode('ascii'),
        'wifi_profile': profile is not None,
        'files': {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                  for name, data in outputs.items()},
        'flash': ['fastboot flash boot flash-boot.img', 'fastboot reboot'],
    }
    outputs['install.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    return outputs


def check_device_trees(section):
    """The boot image's device tree section: one flattened device tree, or
    Android's DT table of them (mkdtimg's format, which the stock image uses),
    from which LK picks one. Each must be a whole flattened device tree."""
    if section[:4] == FDT_MAGIC:
        total = len(section)
        data_start = 0
        trees = [(0, total)]
    elif len(section) >= 32 and struct.unpack_from('>I', section)[0] == DT_TABLE_MAGIC:
        _, total, header, entry, count, entries, _, version = struct.unpack_from('>8I', section)
        if (total > len(section) or header != 32 or entry != 32 or not count or version or
                not header <= entries <= total - count * entry):
            raise PackagingError('stock boot.img has an unsupported DT table')
        data_start = entries + count * entry
        trees = [struct.unpack_from('>2I', section, entries + i * entry)[::-1]
                 for i in range(count)]
    else:
        raise PackagingError('stock boot.img has no flattened device tree')
    for offset, size in trees:
        if offset < data_start or size < 40 or offset + size > total:
            raise PackagingError('stock boot.img has a malformed device tree')
        tree_size, = struct.unpack_from('>I', section, offset + 4)
        if tree_size > min(size, MAX_DTB_SIZE):
            raise PackagingError('stock boot.img has a malformed device tree')
        # The entry can include padding after the FDT's declared total size.
        resources.parse_dtb(section[offset:offset + tree_size], max_size=MAX_DTB_SIZE)


def stock_boot(image):
    """The stock image's header fields and device trees. Only what SOS2 relies
    on is checked: header v2, 2 KiB pages, LK's kernel address, BOOTOPT."""
    if len(image) < PAGE_SIZE or image[:8] != b'ANDROID!':
        raise PackagingError('stock boot.img is not an Android boot image')
    fields = struct.unpack_from(HEADER_V2, image)
    (_, kernel_size, kernel_address, ramdisk_size, ramdisk_address, second_size, second_address,
     tags_address, page_size, version, os_version, name, cmdline, _, extra, dtbo_size, _,
     header_size, dtb_size, dtb_address) = fields
    if (version, page_size, header_size) != (2, PAGE_SIZE, HEADER_SIZE):
        raise PackagingError('stock boot.img is not a header v2 image with 2 KiB pages')
    if kernel_address != KERNEL_ADDRESS:
        raise PackagingError(f'stock boot.img loads its kernel at 0x{kernel_address:08x}, '
                             f'not 0x{KERNEL_ADDRESS:08x}')
    cmdline = cmdline.split(b'\0', 1)[0]
    if BOOTOPT not in cmdline.split():
        raise PackagingError('stock command line lacks ' + BOOTOPT.decode())
    if not dtb_size or second_size or dtbo_size:
        raise PackagingError('stock boot.img must hold a device tree and no second stage')
    offset = PAGE_SIZE + align(kernel_size) + align(ramdisk_size)
    dtb = image[offset:offset + dtb_size]
    if len(dtb) != dtb_size:
        raise PackagingError('stock boot.img ends inside its device tree section')
    check_device_trees(dtb)
    if not KERNEL_ADDRESS < DEBUG_LOGS <= min(dtb_address, ramdisk_address):
        raise PackagingError('stock layout places the device tree or ramdisk over the kernel')
    # A stock image with an AVB footer fills its partition. A trimmed stock
    # image still targets the SM-T220's 32 MiB boot partition.
    partition = BOOT_PARTITION_SIZE
    if image[-64:-60] == b'AVBf':
        partition = min(partition, len(image))
    return {'ramdisk_address': ramdisk_address, 'second_address': second_address,
            'tags_address': tags_address, 'os_version': os_version, 'name': name,
            'cmdline': cmdline, 'extra': extra, 'dtb': dtb, 'dtb_address': dtb_address,
            'partition': partition}


def boot_image(stock, kernel, ramdisk):
    """A header v2 image as mkbootimg writes it, then Samsung's trailer."""
    sections = (kernel, ramdisk, b'', b'', stock['dtb'])
    size = PAGE_SIZE + sum(align(len(section)) for section in sections) + len(SEANDROID)
    if size > stock['partition']:
        raise PackagingError(f'boot.img needs {size} bytes; the boot partition holds '
                             f'{stock["partition"]}')
    digest = hashlib.sha1()
    for section in sections:
        digest.update(section)
        digest.update(struct.pack('<I', len(section)))
    header = struct.pack(HEADER_V2, b'ANDROID!', len(kernel), KERNEL_ADDRESS, len(ramdisk),
                         stock['ramdisk_address'], 0, stock['second_address'], stock['tags_address'],
                         PAGE_SIZE, 2, stock['os_version'], stock['name'], stock['cmdline'],
                         digest.digest(), stock['extra'], 0, 0, HEADER_SIZE, len(stock['dtb']),
                         stock['dtb_address'])
    image = header.ljust(PAGE_SIZE, b'\0')
    for section in sections:
        image += section.ljust(align(len(section)), b'\0')
    image += SEANDROID
    return image


def installed_image(stock, kernel, ramdisk):
    """The installed system: the RAM image with sos2.storage=installed, as on
    the phone (tools/package_redmi2.py's make_install_files). LK ends its long
    command line with the image's; the kernel picks its sos2.* arguments out
    of it (kernel/fdt.c)."""
    cmdline = b'sos2.storage=installed ' + stock['cmdline']
    if len(cmdline) >= 512:
        raise PackagingError('the installed boot image\'s command line exceeds its 512 bytes')
    return boot_image(dict(stock, cmdline=cmdline), kernel, ramdisk)


def disabled_vbmeta(vbmeta):
    """The stock vbmeta image with AVB's verification-disabled flag set."""
    if len(vbmeta) < 256 or vbmeta[:4] != b'AVB0':
        raise PackagingError('stock vbmeta.img is not an AVB vbmeta image')
    authentication_size, auxiliary_size = struct.unpack_from('>QQ', vbmeta, 12)
    if 256 + authentication_size + auxiliary_size > len(vbmeta):
        raise PackagingError('stock vbmeta.img ends inside its declared AVB data')
    flags, = struct.unpack_from('>I', vbmeta, 120)
    return vbmeta[:120] + struct.pack('>I', flags | AVB_VERIFICATION_DISABLED) + vbmeta[124:]


def odin_tar(name, data):
    """A tar of one partition image, as odin4 -a takes it."""
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        info = tarfile.TarInfo(name)
        info.size, info.mode, info.mtime = len(data), 0o644, 0
        tar.addfile(info, io.BytesIO(data))
    return out.getvalue()


def write(directory, outputs):
    directory.mkdir(parents=True, exist_ok=True)
    lock = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX)
        manifest = directory / FLASH_MANIFEST
        staged = [(directory / name, data) for name, data in outputs.items()]
        previous = None
        if 'boot.img' in outputs:
            hashes = {name: hashlib.sha256(outputs[name]).hexdigest() for name in FLASH_FILES}
            staged.append((manifest, json.dumps(hashes).encode()))
        elif manifest.is_file() and not manifest.is_symlink():
            try:
                previous = json.loads(manifest.read_bytes())
            except ValueError as error:
                raise PackagingError(f'invalid image manifest: {manifest}') from error
            if not isinstance(previous, dict) or set(previous) != set(FLASH_FILES):
                raise PackagingError(f'invalid image manifest: {manifest}')
        resources.write_outputs(staged)
        if previous is not None:
            # A stock backup or a replacement copied here after the previous
            # build belongs to the caller. Only remove our own unchanged files.
            for name in FLASH_FILES:
                path = directory / name
                if path.is_file() and not path.is_symlink():
                    with path.open('rb') as stream:
                        if hashlib.file_digest(stream, 'sha256').hexdigest() == previous[name]:
                            path.unlink()
            manifest.unlink()
    finally:
        os.close(lock)
    for name, data in outputs.items():
        print(f'{directory / name}: {len(data)} bytes')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kernel', type=Path, required=True, help='build/gta7lite-arm64/kernel.img')
    parser.add_argument('--resources', type=Path, required=True, help='the arm64 native-web.bundle')
    parser.add_argument('--release', required=True, help="the build's name, shown by the updater")
    parser.add_argument('--stock', type=Path, help="folder with the stock firmware's boot.img and vbmeta.img")
    parser.add_argument('--touch-firmware', type=Path,
                        help="folder with the stock vendor partition's novatek_ts_*_fw.bin")
    parser.add_argument('--wifi-firmware', type=Path,
                        help="folder with the stock vendor partition's soc1_0 firmware files")
    parser.add_argument('--wifi-profile', type=Path,
                        help='a Wi-Fi profile made by tools/redmi2_wifi.py, joined at boot')
    parser.add_argument('--install-dir', type=Path,
                        help='write the installed system and its update here (needs --stock)')
    parser.add_argument('--apps-dir', type=Path, help='write the apps as packages, with index.txt, here')
    parser.add_argument('--jsc', help='JavaScriptCore options for pages, comma-separated (the sos2.jsc= boot argument)')
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        inputs = {args.kernel.resolve(), args.resources.resolve()}
        if args.stock:
            inputs.update((args.stock / name).resolve() for name in ('boot.img', 'vbmeta.img'))
        if args.touch_firmware:
            inputs.update((args.touch_firmware / name).resolve() for name in TOUCH_FIRMWARE)
        if args.wifi_firmware:
            inputs.update((args.wifi_firmware / name).resolve() for name in WIFI_FIRMWARE)
        if args.wifi_profile:
            inputs.add(args.wifi_profile.resolve())
        for name in ('Image.gz', 'resources.sorz', *FLASH_FILES, FLASH_MANIFEST):
            if (args.output_dir / name).resolve() in inputs:
                raise PackagingError(f'output {args.output_dir / name} overlaps an input file')
        kernel = kernel_image(args.kernel.read_bytes())
        touch = touch_firmware(args.touch_firmware) if args.touch_firmware else None
        wifi = wifi_firmware(args.wifi_firmware) if args.wifi_firmware else None
        profile = None
        if args.wifi_profile:
            from redmi2_wifi import WifiError, profile_bytes
            try:
                profile = profile_bytes(json.loads(args.wifi_profile.read_bytes()))
            except (ValueError, WifiError) as error:
                raise PackagingError(f'{args.wifi_profile}: {error}') from error
        if args.install_dir and not args.stock:
            raise PackagingError('the installed system needs the stock boot image: --stock')
        ramdisk, packages = tablet_resources(args.resources.read_bytes(), args.release, touch, wifi, profile)
        outputs = {'Image.gz': kernel, 'resources.sorz': ramdisk}
        if args.stock:
            stock = stock_boot((args.stock / 'boot.img').read_bytes())
            stock = dict(stock, cmdline=resources.with_jsc_options(stock['cmdline'], args.jsc))
            image = boot_image(stock, kernel, ramdisk)
            vbmeta = disabled_vbmeta((args.stock / 'vbmeta.img').read_bytes())
            outputs.update({'boot.img': image, 'boot.tar': odin_tar('boot.img', image),
                            'vbmeta-disabled.tar': odin_tar('vbmeta.img', vbmeta),
                            'flash-boot.img': installed_image(stock, kernel, ramdisk)})
        extra = []
        if args.install_dir:
            extra += [(args.install_dir / name, data)
                      for name, data in install_files(args.release, outputs['flash-boot.img'], profile).items()]
        if args.apps_dir:
            extra += [(args.apps_dir / f'{app_id}.sos2app', package) for app_id, package in packages.items()]
            extra.append((args.apps_dir / 'index.txt', resources.app_index(packages)))
        if any(path.resolve() in inputs for path, _ in extra):
            raise PackagingError('an install or app output overlaps an input file')
        write(args.output_dir, outputs)
        for directory in {path.parent for path, _ in extra}:
            directory.mkdir(parents=True, exist_ok=True)
        if extra:
            resources.write_outputs(extra)
        if args.apps_dir:
            # Packages of apps no longer in the bundle are not this build's.
            for stale in args.apps_dir.glob('*.sos2app'):
                if stale.stem not in packages:
                    stale.unlink()
        for path, data in extra:
            print(f'{path}: {len(data)} bytes')
    except (OSError, PackagingError) as error:
        parser.exit(1, f'{error}\n')
    if not args.stock:
        print('No flash images built: boot.img needs the stock firmware via GTA7LITE_STOCK')
    if not args.touch_firmware:
        print('No touch firmware: the tablet has no touch without GTA7LITE_TOUCH_FIRMWARE')
    if not args.wifi_firmware:
        print('No Wi-Fi firmware: the tablet has no Wi-Fi without GTA7LITE_WIFI_FIRMWARE')


if __name__ == '__main__':
    sys.exit(main())
