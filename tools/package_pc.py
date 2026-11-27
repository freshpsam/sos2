#!/usr/bin/env python3
"""Build a PC's USB stick image and installed system; never access a disk.

A PC starts SOS2 from a GPT stick (docs/x64.md):
  EFI system partition  SOS2's UEFI loader as EFI/BOOT/BOOTX64.EFI, on FAT32
  sos2-system           the system image (include/boot/pc_image.h): the
                        kernel's ELF and the resources, engine included, as
                        a SORZ ramdisk (tools/package_redmi2.py's format),
                        which updates rewrite as the phones' boot partition
  sos2-data             the app store and the data volume
                        (include/system_store.h), which SOS2 formats at its
                        first start
As on the phones, the images build in no apps: the system's updater
installs them, and --apps-dir writes the bundle's apps as x64 packages
with their index.txt. --storage ram makes a stick whose system keeps its
data in RAM only. --install-dir writes what tools/publish_pages.py
publishes: the installed stick image, xz-compressed (sos2-pc.img.xz), the
updater's file (sos2-x64.update: the system image after its 4 KiB
header) and install.json.
Writes to --output-dir:
  sos2-pc.img   the stick, for QEMU (make pc-run) or a USB stick of
                exactly 322 MiB (a larger one boots without storage)
"""

import argparse
import hashlib
import json
import lzma
from pathlib import Path
import struct
import sys
import uuid
import zlib

import package_redmi2 as resources
from package_redmi2 import PackagingError

SECTOR = 512
PAGE = 4096
MIB = 1 << 20
BOARD = b'x64'
INSTALL_FILES = ('sos2-pc.img', 'sos2-x64.update')
# The stick: partitions on mebibyte boundaries after the primary GPT. The
# system partition is the PC's SYSTEM_BOOT_LIMIT (include/system_store.h);
# the data partition holds the app store's 61 MiB and a 131 MiB volume.
ESP_START, ESP_SIZE = 1 * MIB, 64 * MIB
SYSTEM_START, SYSTEM_SIZE = 65 * MIB, 64 * MIB
DATA_START, DATA_SIZE = 129 * MIB, 192 * MIB
STICK_SIZE = DATA_START + DATA_SIZE + MIB
GPT_ENTRIES, GPT_ENTRY = 128, 128
ESP_TYPE = uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b')
SYSTEM_TYPE = uuid.UUID('a0b7863c-00a6-4c31-a459-4351dbd60d96')
DATA_TYPE = uuid.UUID('b794ceb6-7f8b-4a76-a489-ba092cf83ebe')
# Unique GUIDs derive from the release, so a build is reproducible.
GUID_NAMESPACE = uuid.UUID('67d224da-2897-4388-a45d-1e1468cc1879')
# include/boot/pc_image.h
PC_IMAGE_MAGIC = b'SOS2PCIM'
PC_IMAGE_VERSION = 1
PC_IMAGE_HEADER = 4096
PC_IMAGE_FIELDS = '<8sIIQQQQQII256sI'
COMMAND_LINE_MAX = 255
# FAT32 with one-sector clusters: 64 MiB holds the 65,525 clusters FAT32
# needs; 2026-01-01 for every timestamp.
FAT_RESERVED, FAT_COUNT, FAT_EOC = 32, 2, 0x0FFFFFFF
FAT_DATE, FAT_TIME = (2026 - 1980) << 9 | 1 << 5 | 1, 0


def align(value, alignment):
    return (value + alignment - 1) & -alignment


def elf64(program, what, hint):
    """A static x64 executable, or why not."""
    if (len(program) < 64 or program[:7] != b'\x7fELF\x02\x01\x01' or
            struct.unpack_from('<HH', program, 16) != (2, 62)):
        raise PackagingError(f'the PC needs the x64 {what}; {hint}')
    phoff, = struct.unpack_from('<Q', program, 32)
    size, count = struct.unpack_from('<HH', program, 54)
    if size != 56 or not count or phoff + count * size > len(program) or any(
            struct.unpack_from('<I', program, phoff + i * size)[0] in (2, 3) for i in range(count)):
        raise PackagingError(f'the {what} must be static x64, without a dynamic loader')


def pc_resources(bundle, release):
    """The SORZ resources and the apps they leave out, as packages by id: the
    x64 bundle, checked, without its apps, with the release's name and
    a RAM-only storage policy unless the command line names another."""
    files = resources.bundle_files(bundle)
    elf64(files.get(b'/bin/sos2-updater', b''), 'system updater, /bin/sos2-updater',
          'run make ARCH=x64 engine')
    elf64(files.get(b'/bin/sos2-web', b''), 'engine, /bin/sos2-web', 'run make ARCH=x64 engine')
    packages = resources.app_packages(files, elf=resources.ELF_X64)
    for name in [name for name in files if name.startswith(b'/apps/')]:
        del files[name]
    if b'/system/release' in files or b'/system/storage-mode' in files:
        raise PackagingError('the input bundle already names a release or a storage policy')
    files[b'/system/release'] = resources.validate_release(release)
    files[b'/system/storage-mode'] = b'RAM1'
    data = resources.sorb(files)
    if len(data) > 256 * MIB:
        raise PackagingError('expanded native resources exceed the kernel limit')
    return resources.sorz(data), packages


def system_image(kernel, ramdisk, command_line):
    """include/boot/pc_image.h: the header, the kernel's ELF and the
    resources, each on a 4 KiB boundary."""
    elf64(kernel, 'kernel', 'run make pc')
    if len(command_line) > COMMAND_LINE_MAX:
        raise PackagingError(f'the command line exceeds {COMMAND_LINE_MAX} bytes')
    kernel_offset = PC_IMAGE_HEADER
    ramdisk_offset = align(kernel_offset + len(kernel), PAGE)
    size = align(ramdisk_offset + len(ramdisk), PAGE)
    if size > SYSTEM_SIZE:
        raise PackagingError(f'the system image needs {size} bytes; its partition holds {SYSTEM_SIZE}')
    fields = struct.pack(PC_IMAGE_FIELDS, PC_IMAGE_MAGIC, PC_IMAGE_VERSION, PC_IMAGE_HEADER, kernel_offset,
                         len(kernel), ramdisk_offset, len(ramdisk), size, zlib.crc32(kernel),
                         zlib.crc32(ramdisk), command_line, 0)
    header = bytearray(fields.ljust(PC_IMAGE_HEADER, b'\0'))
    struct.pack_into('<I', header, len(fields) - 4, zlib.crc32(header))
    image = bytearray(size)
    image[:PC_IMAGE_HEADER] = header
    image[kernel_offset:kernel_offset + len(kernel)] = kernel
    image[ramdisk_offset:ramdisk_offset + len(ramdisk)] = ramdisk
    return bytes(image)


def gpt(total_sectors, partitions, disk_guid):
    """A protective MBR and the primary and backup GPT for partitions
    [(type, unique GUID, byte offset, size, name)], as {offset: bytes}."""
    entries = bytearray(GPT_ENTRIES * GPT_ENTRY)
    for index, (kind, unique, offset, size, name) in enumerate(partitions):
        struct.pack_into('<16s16sQQQ72s', entries, index * GPT_ENTRY, kind.bytes_le, unique.bytes_le,
                         offset // SECTOR, (offset + size) // SECTOR - 1, 0, name.encode('utf-16-le'))
    entry_sectors = len(entries) // SECTOR
    last = total_sectors - 1

    def header(current, backup, entries_lba):
        fields = struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x00010000, 92, 0, 0, current, backup,
                             2 + entry_sectors, last - 1 - entry_sectors, disk_guid.bytes_le, entries_lba,
                             GPT_ENTRIES, GPT_ENTRY, zlib.crc32(entries))
        return (fields[:16] + struct.pack('<I', zlib.crc32(fields)) + fields[20:]).ljust(SECTOR, b'\0')
    mbr = bytearray(SECTOR)
    struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\x00\x02\x00', 0xee, b'\xff\xff\xff', 1,
                     min(last, 0xffffffff))
    mbr[510:] = b'\x55\xaa'
    return {0: bytes(mbr), SECTOR: header(1, last, 2), 2 * SECTOR: bytes(entries),
            (last - entry_sectors) * SECTOR: bytes(entries), last * SECTOR: header(last, 1, last - entry_sectors)}


def fat_name(name):
    base, _, extension = name.upper().partition('.')
    if not 1 <= len(base) <= 8 or len(extension) > 3:
        raise PackagingError(f'{name} is not an 8.3 name')
    return base.ljust(8).encode('ascii') + extension.ljust(3).encode('ascii')


def fat_entry(name, attributes, cluster, size=0):
    return struct.pack('<11sBBBHHHHHHHI', name, attributes, 0, 0, FAT_TIME, FAT_DATE, FAT_DATE, cluster >> 16,
                       FAT_TIME, FAT_DATE, cluster & 0xffff, size)


def fat32(sectors, hidden, files, volume_id):
    """A FAT32 volume of sectors 512-byte sectors, one per cluster, holding
    files {'EFI/BOOT/BOOTX64.EFI': data}, as {offset: bytes} within it."""
    fat_sectors = 1
    while True:
        clusters = sectors - FAT_RESERVED - FAT_COUNT * fat_sectors
        needed = (4 * (clusters + 2) + SECTOR - 1) // SECTOR
        if needed <= fat_sectors:
            break
        fat_sectors = needed
    if clusters < 65525:
        raise PackagingError('the EFI partition is too small for FAT32')
    fat = [0x0FFFFFF8, FAT_EOC, FAT_EOC]  # media, reserved, root directory
    directories = {'': 2}
    contents = {2: [fat_entry(b'SOS2 EFI   ', 0x08, 0)]}
    regions = {}
    data_start = (FAT_RESERVED + FAT_COUNT * fat_sectors) * SECTOR

    def allocate(count):
        first = len(fat)
        fat.extend(range(first + 1, first + count))
        fat.append(FAT_EOC)
        return first
    for path, data in files.items():
        parts = path.split('/')
        parent = ''
        for part in parts[:-1]:
            here = f'{parent}/{part}'.strip('/')
            if here not in directories:
                cluster = allocate(1)
                directories[here] = cluster
                above = directories[parent]
                contents[cluster] = [fat_entry(b'.          ', 0x10, cluster),
                                     fat_entry(b'..         ', 0x10, 0 if above == 2 else above)]
                contents[above].append(fat_entry(fat_name(part), 0x10, cluster))
            parent = here
        count = max(1, (len(data) + SECTOR - 1) // SECTOR)
        cluster = allocate(count)
        contents[directories[parent]].append(fat_entry(fat_name(parts[-1]), 0x20, cluster, len(data)))
        regions[data_start + (cluster - 2) * SECTOR] = data
    for cluster, entries in contents.items():
        if len(entries) > SECTOR // 32:
            raise PackagingError('a directory outgrew its cluster')
        regions[data_start + (cluster - 2) * SECTOR] = b''.join(entries)
    if len(fat) > clusters + 2:
        raise PackagingError('the files do not fit in the EFI partition')
    table = struct.pack(f'<{len(fat)}I', *fat)
    for copy in range(FAT_COUNT):
        regions[(FAT_RESERVED + copy * fat_sectors) * SECTOR] = table
    boot = bytearray(SECTOR)
    boot[:11] = b'\xeb\x58\x90MSWIN4.1'
    struct.pack_into('<HBHBHHBHHHII', boot, 11, SECTOR, 1, FAT_RESERVED, FAT_COUNT, 0, 0, 0xf8, 0, 63, 255,
                     hidden, sectors)
    struct.pack_into('<IHHIHH12sBBBI11s8s', boot, 36, fat_sectors, 0, 0, 2, 1, 6, bytes(12), 0x80, 0, 0x29,
                     volume_id, b'SOS2 EFI   ', b'FAT32   ')
    boot[510:] = b'\x55\xaa'
    info = bytearray(SECTOR)
    struct.pack_into('<I', info, 0, 0x41615252)
    struct.pack_into('<III', info, 484, 0x61417272, clusters + 2 - len(fat), len(fat))
    struct.pack_into('<I', info, 508, 0xaa550000)
    regions.update({0: bytes(boot), SECTOR: bytes(info), 6 * SECTOR: bytes(boot), 7 * SECTOR: bytes(info)})
    return regions


def stick(loader, image, release):
    """The stick image: GPT, the EFI partition with the loader, the system
    image in sos2-system and an empty sos2-data, as {offset: bytes}."""
    def guid(name):
        return uuid.uuid5(GUID_NAMESPACE, f'{release}/{name}')
    regions = gpt(STICK_SIZE // SECTOR, [
        (ESP_TYPE, guid('esp'), ESP_START, ESP_SIZE, 'EFI system partition'),
        (SYSTEM_TYPE, guid('system'), SYSTEM_START, SYSTEM_SIZE, 'sos2-system'),
        (DATA_TYPE, guid('data'), DATA_START, DATA_SIZE, 'sos2-data')], guid('disk'))
    volume = zlib.crc32(release.encode())
    for offset, data in fat32(ESP_SIZE // SECTOR, ESP_START // SECTOR,
                              {'EFI/BOOT/BOOTX64.EFI': loader}, volume).items():
        regions[ESP_START + offset] = data
    regions[SYSTEM_START] = image
    return regions


def write_stick(path, regions):
    """The stick image file, its unwritten parts sparse zeros."""
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(path.name + '.partial')
    with partial.open('wb') as stream:
        stream.truncate(STICK_SIZE)
        for offset, data in sorted(regions.items()):
            stream.seek(offset)
            stream.write(data)
    partial.replace(path)


def stick_bytes(regions):
    image = bytearray(STICK_SIZE)
    for offset, data in regions.items():
        image[offset:offset + len(data)] = data
    return bytes(image)


def install_files(release, image, stick_image):
    """What tools/publish_pages.py publishes: the stick image xz-compressed
    (mostly zeros: about the size of the system), the updater's file, the
    system image after update_header, and install.json, which lists both
    with their sizes and SHA-256s and how a first install goes."""
    compressed = lzma.compress(stick_image, preset=6)
    update = resources.update_header(release, image, BOARD) + image
    outputs = {'sos2-pc.img.xz': compressed, 'sos2-x64.update': update}
    manifest = {'release': release, 'board': BOARD.decode('ascii'), 'wifi_firmware': False, 'wifi_profile': False,
                'files': {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                          for name, data in outputs.items()},
                'first_install': f'a first install writes sos2-pc.img, uncompressed, to a USB stick of exactly '
                                 f'{STICK_SIZE // MIB} MiB (everything on it is lost) and starts the PC from it, '
                                 'Secure Boot off; so far it runs only on QEMU\'s emulated PC (make pc-run):',
                'flash': ['xz -dk sos2-pc.img.xz', '(then write sos2-pc.img to the stick with a disk imaging tool)']}
    outputs['install.json'] = (json.dumps(manifest, indent=2) + '\n').encode('utf-8')
    return outputs


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kernel', type=Path, required=True, help='build/pc-x64/kernel.elf')
    parser.add_argument('--loader', type=Path, required=True, help='build/pc-x64/BOOTX64.EFI')
    parser.add_argument('--resources', type=Path, required=True, help='the x64 native-web.bundle')
    parser.add_argument('--release', required=True)
    parser.add_argument('--storage', choices=('ram', 'installed'), default='installed')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--apps-dir', type=Path, help='write the x64 app packages and index.txt here')
    parser.add_argument('--install-dir', type=Path, help='write the installed system for make publish here')
    args = parser.parse_args(argv)
    try:
        ramdisk, packages = pc_resources(args.resources.read_bytes(), args.release)
        kernel, loader = args.kernel.read_bytes(), args.loader.read_bytes()
        line = b'sos2.storage=' + args.storage.encode()
        write_stick(args.output_dir / 'sos2-pc.img', stick(loader, system_image(kernel, ramdisk, line),
                                                            args.release))
        if args.install_dir:
            installed = system_image(kernel, ramdisk, b'sos2.storage=installed')
            outputs = install_files(args.release, installed, stick_bytes(stick(loader, installed, args.release)))
            args.install_dir.mkdir(parents=True, exist_ok=True)
            resources.write_outputs([(args.install_dir / name, data) for name, data in outputs.items()])
        if args.apps_dir:
            args.apps_dir.mkdir(parents=True, exist_ok=True)
            resources.write_outputs([(args.apps_dir / f'{app_id}.sos2app', package)
                                     for app_id, package in sorted(packages.items())] +
                                    [(args.apps_dir / 'index.txt', resources.app_index(packages))])
    except (OSError, PackagingError) as error:
        sys.exit(f'package_pc: {error}')
    print('PC stick:', args.output_dir / 'sos2-pc.img')


if __name__ == '__main__':
    main()
