#!/usr/bin/env python3
"""Build lk2nd boot images and the installed system; never access a device.

The source-backed contract and device prerequisites are in docs/redmi2.md.
Only a verified Redmi 2 ELF/raw pair and a minimal wt88047 DTB are accepted.
With --install-dir, the same compressed resources also become the installed
system: flash-boot.img for lk2nd's boot partition, flash-cache.img for the
cache partition's first slot, and sos2-redmi2.update, which the system's
updater, /bin/sos2-updater, downloads (docs/redmi2.md#installing). The phone's
images build in no apps; with --apps-dir, the bundle's apps become packages,
<id>.sos2app, with index.txt, which the updater installs one by one
(docs/redmi2.md#apps).
"""

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import lzma
import zlib


LK2ND_REVISION = "3e86db90765468c55e1be507cb897efd20876b71"
ARM32_KERNEL_ADDRESS = 0x80008000
TAGS_ADDRESS = 0x81E00000
RAMDISK_ADDRESS = 0x82000000
# lk2nd copies the ramdisk before answering `fastboot boot`, and on MSM8916 it
# maps only the first 90 MiB of RAM for kernel and ramdisk
# (platform/msm8916/platform.c, platform_init_mmu_mappings). A ramdisk past
# 0x85A00000 faults there: fastboot reports usb_read failed and the phone
# restarts.
RAMDISK_LIMIT = 0x85A00000
PAGE_SIZE = 2048
# SORZ v2 resources: one raw LZMA stream with an end marker, which the kernel
# decodes with third_party/lzma (kernel/protection/boot_bundle.c). lc=3, lp=0,
# pb=2 are LZMA's defaults; the kernel accepts lc+lp <= 3. The header carries
# the properties byte; the kernel sizes the dictionary to the whole output.
LZMA_FILTERS = [{'id': lzma.FILTER_LZMA1, 'preset': 9 | lzma.PRESET_EXTREME,
                 'dict_size': 64 * 1024 * 1024, 'lc': 3, 'lp': 0, 'pb': 2}]
LZMA_PROPERTIES = (LZMA_FILTERS[0]['pb'] * 5 + LZMA_FILTERS[0]['lp']) * 9 + LZMA_FILTERS[0]['lc']
SORZ_MAGIC = 0x5a524f53
MAX_DTB_SIZE = 1024 * 1024
CMDLINE = b"lk2nd.pass-simplefb=xrgb8888,autorefresh"
DISPLAY_CMDLINES = {
    "xrgb8888": CMDLINE,
    "native": b"lk2nd.pass-simplefb=autorefresh",
}
# The installed system (include/system_store.h, kernel/protection/system_update.c):
# lk2nd keeps its first 512 KiB of the 32 MiB boot partition and boots the
# image after it; the cache partition holds two slots for the compressed
# resources, the first 1 MiB in, each a 4 KiB header and the SORZ data. The
# boot image names its slot's SHA-256 with sos2.system=.
BOOT_IMAGE_LIMIT = 4 * 1024 * 1024
SLOT_A_OFFSET = 1024 * 1024
SLOT_HEADER = 4096
UPDATE_HEADER = 4096
BOARD = b"redmi2-wt88047"
RELEASE_MAX = 47
INSTALL_FILES = ("flash-boot.img", "flash-cache.img", "sos2-redmi2.update")
# App packages (struct app_package in include/app_store.h): a 512-byte header,
# the 48x48 ARGB icon and the program, at most one app store half less its
# 4 KiB header.
APP_PACKAGE_HEADER = 512
APP_ICON_BYTES = 48 * 48 * 4
APP_PACKAGE_MAX = 6144 * 512 - 4096
APP_INDEX = b"SOS2 apps 1"
ABI = int(re.search(r"#define USER_ABI_VERSION (\d+)",
                    (Path(__file__).resolve().parents[1] / "include/user_abi.h").read_text()).group(1))


class PackagingError(ValueError):
    """Invalid or incompatible input; no image should be produced."""


def align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def validate_arm32_kernel(kernel, elf):
    """Validate a flat ARM-state image with the zImage header used to locate its DTB.

    lk2nd 23.1 forces DDR_START+0x8000 for ARM32 and reads the appended
    DTB offset at 0x2c (platform/msm8916/include/platform/iomap.h and
    platform/msm_shared/dev_tree.c). The payload has no decompressor.
    """
    if len(kernel) < 64 or struct.unpack_from("<I", kernel, 0x24)[0] != 0x016F2818:
        raise PackagingError("kernel must contain an ARM32 zImage-shaped header")
    if struct.unpack_from("<II", kernel, 0x28) != (0, len(kernel)):
        raise PackagingError("ARM32 header must identify the raw file end as its appended DTB offset")
    if kernel[8:16] != bytes(8) or kernel[56:60] == b"ARM\x64":
        raise PackagingError("ARM32 header conflicts with lk2nd's ARM64 header inspection")
    branch = struct.unpack_from("<I", kernel)[0]
    displacement = branch & 0xFFFFFF
    if displacement & 0x800000:
        displacement -= 1 << 24
    if branch >> 24 != 0xEA or not 64 <= 8 + displacement * 4 < len(kernel):
        raise PackagingError("ARM32 entry must branch in ARM state past its image header")
    if len(elf) < 52 or elf[:7] != b"\x7fELF\x01\x01\x01":
        raise PackagingError("--elf must be a little-endian ELF32 file for ARM32")
    fields = struct.unpack_from("<HHIIIIIHHHHHH", elf, 16)
    elf_type, machine, version, entry, phoff = fields[:5]
    flags, ehsize, phentsize, phnum = fields[6:10]
    if (elf_type, machine, version, entry, ehsize) != (2, 40, 1, ARM32_KERNEL_ADDRESS, 52):
        raise PackagingError("ELF is not an ARM executable linked for Redmi 2 at 0x80008000")
    if flags & 0xFF000000 != 0x05000000 or flags & 0x400:
        raise PackagingError("ARM32 ELF must use EABI5 without the hard-float calling convention")
    if phentsize != 32 or not 1 <= phnum <= 64 or phoff + phentsize * phnum > len(elf):
        raise PackagingError("invalid ELF program header table")
    expected = bytearray(len(kernel))
    written = bytearray(len(kernel))
    first, file_end, memory_end = None, 0, ARM32_KERNEL_ADDRESS
    for index in range(phnum):
        kind, offset, vaddr, paddr, filesz, memsz, _, _ = struct.unpack_from(
            "<8I", elf, phoff + index * phentsize
        )
        if kind != 1 or memsz == 0:
            continue
        if (vaddr != paddr or paddr < ARM32_KERNEL_ADDRESS or filesz > memsz
                or paddr + memsz > TAGS_ADDRESS or offset + filesz > len(elf)):
            raise PackagingError("ARM32 ELF load segment is outside the kernel's reserved allocation")
        memory_end = max(memory_end, paddr + memsz)
        if not filesz:
            continue
        start = paddr - ARM32_KERNEL_ADDRESS
        end = start + filesz
        if end > len(kernel) or any(written[start:end]):
            raise PackagingError("ELF file segments overlap or exceed the raw image")
        expected[start:end] = elf[offset:offset + filesz]
        written[start:end] = b"\x01" * filesz
        first = paddr if first is None else min(first, paddr)
        file_end = max(file_end, end)
    if first != ARM32_KERNEL_ADDRESS or file_end != len(kernel) or expected != kernel:
        raise PackagingError("raw kernel does not match the supplied Redmi 2 ELF")
    return align(memory_end - ARM32_KERNEL_ADDRESS, PAGE_SIZE)


def parse_dtb(data):
    """Parse bounded FDT v17 data, rejecting ambiguous nodes/properties/trailers."""
    if len(data) < 40 or len(data) > MAX_DTB_SIZE:
        raise PackagingError("DTB size is outside the accepted range")
    magic, total, off_struct, off_strings, off_reserve, version, last, _, strings_len, struct_len = (
        struct.unpack_from(">10I", data)
    )
    if magic != 0xD00DFEED or total != len(data) or version != 17 or last > 17:
        raise PackagingError("expected exactly one complete FDT v17 blob")
    if (off_reserve < 40 or off_reserve % 8 or off_struct % 4
            or off_struct < off_reserve + 16 or struct_len < 12
            or off_struct + struct_len > off_strings
            or off_strings + strings_len > total):
        raise PackagingError("invalid DTB block layout")
    cursor = off_reserve
    while cursor + 16 <= off_struct:
        address, size = struct.unpack_from(">QQ", data, cursor)
        cursor += 16
        if not address and not size:
            break
    else:
        raise PackagingError("DTB memory reservation map is unterminated")
    strings = data[off_strings:off_strings + strings_len]
    cursor, limit = off_struct, off_struct + struct_len
    nodes, stack = {}, []
    ended = False
    while cursor + 4 <= limit:
        token = struct.unpack_from(">I", data, cursor)[0]
        cursor += 4
        if token == 1:
            end = data.find(b"\0", cursor, limit)
            if end < 0:
                raise PackagingError("unterminated DTB node name")
            try:
                name = data[cursor:end].decode("ascii")
            except UnicodeDecodeError as exc:
                raise PackagingError("non-ASCII DTB node name") from exc
            if "/" in name or (not stack and (name or nodes)) or (stack and not name):
                raise PackagingError("invalid DTB node name or multiple roots")
            path = "/" if not stack else stack[-1].rstrip("/") + "/" + name
            if path in nodes or len(stack) >= 32:
                raise PackagingError("duplicate or too deeply nested DTB node")
            nodes[path] = {}
            stack.append(path)
            cursor = align(end + 1, 4)
        elif token == 2:
            if not stack:
                raise PackagingError("unbalanced DTB node end")
            stack.pop()
        elif token == 3:
            if not stack or cursor + 8 > limit:
                raise PackagingError("invalid DTB property header")
            length, nameoff = struct.unpack_from(">II", data, cursor)
            cursor += 8
            nameend = strings.find(b"\0", nameoff)
            if nameoff >= len(strings) or nameend < 0 or cursor + length > limit:
                raise PackagingError("invalid DTB property bounds")
            try:
                name = strings[nameoff:nameend].decode("ascii")
            except UnicodeDecodeError as exc:
                raise PackagingError("non-ASCII DTB property name") from exc
            if not name or name in nodes[stack[-1]]:
                raise PackagingError("empty or duplicate DTB property name")
            nodes[stack[-1]][name] = data[cursor:cursor + length]
            cursor = align(cursor + length, 4)
        elif token == 4:
            continue
        elif token == 9:
            if stack or cursor != limit:
                raise PackagingError("DTB structure ends early or contains trailing data")
            ended = True
            break
        else:
            raise PackagingError("unknown DTB token")
    if not ended or "/" not in nodes:
        raise PackagingError("incomplete DTB structure")
    return nodes


def validate_dtb(data):
    nodes = parse_dtb(data)
    root = nodes["/"]
    if root.get("compatible") != b"wingtech,wt88047\0qcom,msm8916\0":
        raise PackagingError("DTB must target wingtech,wt88047 and qcom,msm8916")
    model = root.get("model", b"")
    if not model.endswith(b"\0") or b"Redmi 2" not in model:
        raise PackagingError("DTB must identify the Redmi 2 model")
    for path in ("/", "/chosen", "/reserved-memory"):
        props = nodes.get(path, {})
        if any(props.get(prop) != struct.pack(">I", 2)
               for prop in ("#address-cells", "#size-cells")):
            raise PackagingError(f"{path} must use two address and size cells")
        if path != "/" and props.get("ranges") != b"":
            raise PackagingError(f"{path} needs explicit empty ranges")
    memory = [props for path, props in nodes.items()
              if path in ("/memory", "/memory@80000000")]
    if (len(memory) != 1 or memory[0].get("device_type") != b"memory\0"
            or memory[0].get("reg") != struct.pack(">4I", 0, 0x80000000, 0, 0)):
        raise PackagingError("DTB needs the SMEM-patched memory placeholder from boards/redmi2/boot.dts")
    if "/lk2nd" in nodes or "/soc/qcom,ion" in nodes:
        raise PackagingError("downstream or lk2nd device trees disable the intended handoff")
    if any(prop in root for prop in ("qcom,msm-id", "qcom,board-id", "qcom,pmic-id")):
        raise PackagingError("DTB must use lk2nd's single non-skales DTB fallback")
    if "bootargs" in nodes["/chosen"]:
        raise PackagingError("DTB bootargs must be absent; the packager sets the boot mode")
    for props in nodes.values():
        if b"simple-framebuffer" in props.get("compatible", b"").split(b"\0"):
            raise PackagingError("framebuffer address must be supplied by lk2nd at runtime")


def unpack_resources(packed):
    """The expanded SORB of a SORZ v2 ramdisk, decoded as the kernel does: with
    the header's properties byte and sizes, checked against its CRC-32s."""
    if len(packed)<=32:raise PackagingError('truncated SORZ ramdisk')
    magic,version,header,size,expanded,crc,expanded_crc,properties=struct.unpack_from('<8I',packed)
    lc,lp,pb=properties%9,properties//9%5,properties//45
    if ((magic,version,header,size)!=(SORZ_MAGIC,2,32,len(packed)-32) or not 16<=expanded<=256*1024*1024 or
            pb>4 or lc+lp>3 or zlib.crc32(packed[32:])!=crc):
        raise PackagingError('invalid SORZ v2 ramdisk header')
    try:
        data=lzma.decompress(packed[32:],format=lzma.FORMAT_RAW,
                             filters=[{'id':lzma.FILTER_LZMA1,'dict_size':expanded,'lc':lc,'lp':lp,'pb':pb}])
    except lzma.LZMAError as error:
        raise PackagingError('SORZ ramdisk does not decode: '+str(error)) from error
    if len(data)!=expanded or zlib.crc32(data)!=expanded_crc:
        raise PackagingError('SORZ ramdisk does not decode to its recorded size and CRC-32')
    return data


def pack_resources(data,rtc_anchor=None,extra_files=None,ram_only=False,release=None):
    """Bounded SORB -> SORZ transport with optional private board resources."""
    return compress_resources(resource_bundle(data,rtc_anchor,extra_files,ram_only,release))


def validate_release(release):
    """The build's name, shown by the updater: 1-47 printable ASCII characters."""
    if (not isinstance(release, str) or not 1 <= len(release) <= RELEASE_MAX
            or not all(" " <= c <= "~" for c in release)):
        raise PackagingError(f"release must be 1-{RELEASE_MAX} printable ASCII characters")
    return release.encode("ascii")


def bundle_files(data):
    """A SORB's files by name, checked as the kernel checks them."""
    if len(data)<16 or len(data)>256*1024*1024:
        raise PackagingError('native resource bundle size is invalid')
    magic,version,size,count=struct.unpack_from('<4I',data)
    if magic!=0x42524f53 or version!=1 or size!=len(data) or not 1<=count<=128 or 16+count*136>size:
        raise PackagingError('invalid native SORB resource bundle')
    files={}
    for i in range(count):
        name,offset,length=struct.unpack_from('<128sII',data,16+i*136)
        if b'\0' not in name or not name.startswith(b'/') or offset<16+count*136 or offset>size or length>size-offset:
            raise PackagingError('invalid native resource entry')
        name=name.split(b'\0',1)[0]
        if name in files:raise PackagingError('duplicate native resource entry')
        files[name]=data[offset:offset+length]
    return files


def app_ids(files):
    """The ids of the bundle's apps."""
    ids = []
    for name in files:
        match = re.fullmatch(rb"/apps/([a-z0-9-]{1,15})/program", name)
        if match:
            ids.append(match.group(1).decode("ascii"))
    return sorted(ids)


def app_package(app_id, name, icon, program, abi=ABI):
    """An app's .sos2app: header, icon and program (include/app_store.h)."""
    if not re.fullmatch(r"[a-z0-9-]{1,15}", app_id):
        raise PackagingError(f"app id {app_id!r} is not 1-15 lowercase letters, digits and hyphens")
    if not 1 <= len(name) <= 12 or not all(32 <= c < 127 for c in name):
        raise PackagingError(f"app {app_id}: the name must be 1-12 printable ASCII characters")
    if len(icon) != APP_ICON_BYTES or len(program) < 52 or program[:7] != b"\x7fELF\x01\x01\x01":
        raise PackagingError(f"app {app_id}: needs a 48x48 icon and an ARM32 ELF program")
    payload = icon + program
    fields = struct.pack("<8sII16s16sIIIII32s", b"SOS2APP1", 1, abi, app_id.encode("ascii"), name,
                         APP_PACKAGE_HEADER, len(icon), APP_PACKAGE_HEADER + len(icon), len(program),
                         APP_PACKAGE_HEADER + len(payload), hashlib.sha256(payload).digest())
    package = (fields + struct.pack("<I", zlib.crc32(fields))).ljust(APP_PACKAGE_HEADER, b"\0") + payload
    if len(package) > APP_PACKAGE_MAX:
        raise PackagingError(f"app {app_id} needs {len(package)} bytes; a package holds {APP_PACKAGE_MAX}")
    return package


def app_packages(files):
    """The bundle's apps, as packages by id."""
    return {app_id: app_package(app_id, files[f"/apps/{app_id}/name".encode()],
                                files[f"/apps/{app_id}/icon".encode()],
                                files[f"/apps/{app_id}/program".encode()])
            for app_id in app_ids(files)}


def app_index(packages):
    """index.txt: a line naming the format, then one per app: id, ABI, size,
    SHA-256 and name, as the updater reads them."""
    lines = [APP_INDEX]
    for app_id, package in sorted(packages.items()):
        name = package[32:48].split(b"\0", 1)[0]
        lines.append(b"%s %d %d %s %s" % (app_id.encode("ascii"), ABI, len(package),
                                          hashlib.sha256(package).hexdigest().encode("ascii"), name))
    return b"\n".join(lines) + b"\n"


def resource_bundle(data,rtc_anchor=None,extra_files=None,ram_only=False,release=None,strip_apps=False):
    """The SORB the phone expands: a checked native bundle plus optional private
    board resources and the release name, /system/release. strip_apps leaves
    out every app, for the phone, which then needs the system's updater."""
    files=bundle_files(data)
    stripped=False
    if strip_apps:
        updater=files.get(b'/bin/sos2-updater',b'')
        if len(updater)<52 or updater[:7]!=b'\x7fELF\x01\x01\x01':
            raise PackagingError('the phone needs the system updater, /bin/sos2-updater; run make engine')
        for app_id in app_ids(files):
            for name in [name for name in files if name.startswith(f'/apps/{app_id}/'.encode())]:
                del files[name]
                stripped=True
    if b'/system/storage-mode' in files:
        raise PackagingError('select storage mode explicitly; input bundle already contains a storage policy')
    if b'/system/release' in files:
        raise PackagingError('input bundle already names a release')
    if ram_only:files[b'/system/storage-mode']=b'RAM1'
    if release is not None:files[b'/system/release']=validate_release(release)
    engine=files.get(b'/bin/sos2-web',b'')
    if len(engine)<52 or engine[:7]!=b'\x7fELF\x01\x01\x01' or struct.unpack_from('<HH',engine,16)!=(2,40):
        raise PackagingError('bundle requires the native ARM32 /bin/sos2-web executable')
    phoff,=struct.unpack_from('<I',engine,28);entry_size,entries=struct.unpack_from('<HH',engine,42)
    if entry_size!=32 or not entries or phoff+entries*entry_size>len(engine) or any(
            struct.unpack_from('<I',engine,phoff+i*entry_size)[0] in (2,3) for i in range(entries)):
        raise PackagingError('browser must be static native ARM32, without a dynamic loader')
    if rtc_anchor is not None:
        epoch,counter=rtc_anchor
        # Match the kernel's accepted range: 2000-01-01 <= t < 2100-01-01.
        if not 946684800<=epoch<4102444800 or not 0<=counter<=0xffffffff:
            raise PackagingError('RTC anchor requires Unix seconds (2000 <= t < 2100) and the observed 32-bit RTC counter')
        files[b'/system/rtc-anchor']=struct.pack('<4IQ',0x41435452,1,counter,0,epoch)
    if extra_files:
        for name,content in extra_files.items():
            radio=name==b'/system/wifi-profile' or re.fullmatch(rb'/firmware/wcnss/(?:wcnss\.(?:mdt|b[0-9]{2})|WCNSS_qcom_wlan_nv\.bin)',name)
            adreno=re.fullmatch(rb'/firmware/adreno/(?:a300_(?:pm4|pfp)\.fw|LICENSE\.qcom\.txt|NOTICE\.txt)',name)
            if not radio and not adreno:raise PackagingError('unexpected board resource')
            if name in files:raise PackagingError('board resources already exist in the input bundle')
            if len(name)>=128 or not content:raise PackagingError('invalid board resource')
            files[name]=content
    gpu_files={name:content for name,content in files.items() if name.startswith(b'/firmware/adreno/')}
    if gpu_files:
        from adreno_firmware import FILES as GPU_FILES, validate as validate_gpu
        expected={('/firmware/adreno/'+name).encode() for name in GPU_FILES}
        if set(gpu_files)!=expected:raise PackagingError('Adreno requires both firmware files and their notices')
        try:
            for name,content in gpu_files.items():validate_gpu(name.decode().rsplit('/',1)[1],content)
        except ValueError as error:raise PackagingError(str(error)) from error
    if rtc_anchor is not None or extra_files or ram_only or release is not None or stripped:
        if len(files)>128:raise PackagingError('no room for board resources in the resource table')
        table=bytearray();payload=bytearray();offset=align(16+136*len(files),4096)
        for name,content in files.items():
            table+=struct.pack('<128sII',name,offset+len(payload),len(content))
            payload+=content;payload+=bytes(align(len(payload),4096)-len(payload))
        data=(struct.pack('<4I',0x42524f53,1,offset+len(payload),len(files))+table).ljust(offset,b'\0')+payload
    if len(data)>256*1024*1024:
        raise PackagingError('expanded native resources exceed the kernel limit')
    return data


def compress_resources(data):
    """The SORZ v2 ramdisk for an expanded SORB, checked by decoding it again."""
    compressed=lzma.compress(data,format=lzma.FORMAT_RAW,filters=LZMA_FILTERS)
    header=struct.pack('<8I',SORZ_MAGIC,2,32,len(compressed),len(data),
                       zlib.crc32(compressed),zlib.crc32(data),LZMA_PROPERTIES)
    packed=header+compressed
    if unpack_resources(packed)!=data:
        raise PackagingError('compressed resources do not decode to the bundle')
    if align(len(packed),PAGE_SIZE)>RAMDISK_LIMIT-RAMDISK_ADDRESS:
        raise PackagingError(f'compressed resources need {align(len(packed),PAGE_SIZE)} bytes; lk2nd can load '
                             f'{RAMDISK_LIMIT-RAMDISK_ADDRESS} bytes of ramdisk at 0x{RAMDISK_ADDRESS:08x}')
    return packed


def boot_payload(kernel, elf, dtb):
    """The checked kernel with its DTB appended, and the kernel's allocation."""
    image_size = validate_arm32_kernel(kernel, elf)
    validate_dtb(dtb)
    payload = kernel + dtb
    # lk2nd copies the appended DTB and page padding as part of the raw payload.
    if align(len(payload), PAGE_SIZE) > image_size:
        raise PackagingError("ARM32 allocation must include lk2nd's rounded kernel and DTB copy size")
    return payload, image_size


def android_image(payload, cmdline, ramdisk):
    """An Android boot v0 image of 2 KiB pages: header, payload, ramdisk."""
    if len(cmdline) >= 512:
        raise PackagingError("boot command line exceeds the Android header's 512 bytes")
    digest = hashlib.sha1()
    digest.update(payload)
    digest.update(struct.pack("<I", len(payload)))
    digest.update(ramdisk)
    digest.update(struct.pack("<I", len(ramdisk)))
    digest.update(struct.pack("<I", 0))  # Empty second-stage payload.
    header = struct.pack(
        "<8s10I16s512s32s1024s", b"ANDROID!", len(payload), ARM32_KERNEL_ADDRESS,
        len(ramdisk), RAMDISK_ADDRESS, 0, 0x80F00000, TAGS_ADDRESS, PAGE_SIZE, 0, 0,
        b"sos2-redmi2", cmdline, digest.digest(), b"",
    )
    image = header.ljust(PAGE_SIZE, b"\0") + payload
    image = image.ljust(align(len(image), PAGE_SIZE), b"\0")
    image += ramdisk
    return image.ljust(align(len(image), PAGE_SIZE), b"\0")


def android_ramdisk(image):
    """The ramdisk of an image from android_image."""
    kernel_size, _, ramdisk_size = struct.unpack_from("<3I", image, 8)
    start = PAGE_SIZE + align(kernel_size, PAGE_SIZE)
    return image[start:start + ramdisk_size]


def make_boot_image(kernel, elf, dtb, display_format="xrgb8888", arch="arm32", resources=None,rtc_anchor=None,extra_files=None,ram_only=False,release=None,strip_apps=False):
    if display_format not in DISPLAY_CMDLINES:
        raise PackagingError("display format must be xrgb8888 or native")
    cmdline = DISPLAY_CMDLINES[display_format]
    if resources is not None:
        # MSM8916 forces ramdisks to 0x82000000; the full browser crosses
        # MIPI_FB_ADDR (0x83200000). Ask lk2nd to move scanout before handing
        # control to us. Its simplefb hook relocates to scratch + 256 - 10 MiB,
        # publishes/reserves the new address, and leaves the ramdisk intact.
        # Merely changing the Android header's ramdisk address cannot fix this:
        # lk2nd overwrites that address. SOS2 still validates the actual DTB.
        cmdline += b",relocate"
    if ram_only:
        if arch!='arm32' or resources is None:
            raise PackagingError('RAM-only mode requires the protected ARM32 browser bundle')
        cmdline+=b' sos2.storage=ram'
    if arch != 'arm32':
        raise PackagingError('only the ARM32 browser kernel is supported')
    payload, image_size = boot_payload(kernel, elf, dtb)
    if rtc_anchor is not None and resources is None:raise PackagingError('RTC calibration requires a resource bundle')
    if extra_files and resources is None:raise PackagingError('Wi-Fi provisioning requires native browser resources')
    if release is not None and resources is None:raise PackagingError('a release name requires a resource bundle')
    expanded=resource_bundle(resources,rtc_anchor,extra_files,ram_only,release,strip_apps) if resources is not None else None
    ramdisk=compress_resources(expanded) if expanded is not None else b''
    kernel_address = ARM32_KERNEL_ADDRESS
    image_format = "Android boot v0; uncompressed ARM32 zImage-shaped image followed by one DTB"
    entry_requirement = "32-bit ARM-state entry with device tree in r2"
    image = android_image(payload, cmdline, ramdisk)
    manifest = {
        "status": "built",
        "target": "Redmi 2 wt88047 / lk2nd-msm8916",
        "lk2nd_source_revision": LK2ND_REVISION,
        "architecture": arch,
        "format": image_format,
        "kernel_address": f"0x{kernel_address:08x}",
        "dtb_address": f"0x{TAGS_ADDRESS:08x}",
        "kernel_allocation_bytes": image_size,
        "kernel_file_bytes": len(kernel),
        "dtb_bytes": len(dtb),
        "boot_image_bytes": len(image),
        "page_size": PAGE_SIZE,
        "cmdline": cmdline.decode("ascii"),
        "framebuffer_relocation": resources is not None,
        "storage_mode": "ram" if ram_only else "disk",
        "sha256": {name: hashlib.sha256(value).hexdigest()
                   for name, value in (("kernel", kernel), ("elf", elf), ("dtb", dtb), ("boot_image", image))},
        "requirements": ["matching device and working lk2nd-msm8916 verified separately",
                         entry_requirement,
                         "working bootloader display and compatible panel",
                         "stock recovery materials verified before any device writes"],
    }
    if resources is not None:
        manifest['resources']={'format':'SORZ v2 / raw LZMA / CRC32',
            'compressed_bytes':len(ramdisk),'expanded_bytes':len(expanded),
            'ramdisk_address':f'0x{RAMDISK_ADDRESS:08x}',
            'ramdisk_end':f'0x{RAMDISK_ADDRESS+len(ramdisk):08x}',
            'expanded_address':'selected from validated free RAM at or above 0x90000000',
            'source_sha256':hashlib.sha256(resources).hexdigest(),
            'sha256':hashlib.sha256(expanded).hexdigest(),
            'transport_sha256':hashlib.sha256(ramdisk).hexdigest()}
        if extra_files:
            for kind,prefix in (('wifi_resources',b'/firmware/wcnss/'),('adreno_resources',b'/firmware/adreno/')):
                # The Wi-Fi profile holds the SSID, station MAC and SAE password
                # or PMK. An unsalted digest would let anyone holding this
                # manifest test password guesses offline, so record its size only.
                entries={name.decode('ascii'):{'bytes':len(content)}|({} if name==b'/system/wifi-profile' else
                                                                    {'sha256':hashlib.sha256(content).hexdigest()})
                         for name,content in extra_files.items() if name.startswith(prefix) or (kind=='wifi_resources' and name==b'/system/wifi-profile')}
                if entries:manifest['resources'][kind]=entries
        if rtc_anchor is not None:manifest['resources']['rtc_anchor']={'unix_seconds':rtc_anchor[0],'rtc_counter':rtc_anchor[1]}
        manifest['requirements']+=['protected kernel with SORZ decoding',
            'lk2nd continuous-splash framebuffer relocation support',
            'reported RAM and live framebuffer must not overlap the ramdisk or expanded resources']
    if release is not None:manifest['release']=release
    if resources is not None:
        manifest['resources']['apps']=app_ids(bundle_files(expanded))
        if strip_apps:manifest['resources']['apps_left_out']=app_ids(bundle_files(resources))
    return image, manifest


def slot_header(system):
    """A cache-partition slot's first 4 KiB: magic, version, data offset and
    size, a reserved word, the data's SHA-256 and a CRC-32 of those 56 bytes
    (struct system_slot in include/system_store.h)."""
    fields = struct.pack("<8sIIII32s", b"SOS2SLOT", 1, SLOT_HEADER, len(system), 0,
                         hashlib.sha256(system).digest())
    return (fields + struct.pack("<I", zlib.crc32(fields))).ljust(SLOT_HEADER, b"\0")


def update_header(release, boot, system):
    """sos2-redmi2.update's first 4 KiB (struct system_update_header in
    include/system_store.h): the release and board, where the boot image and
    the compressed resources follow with their sizes and SHA-256s, the file's
    size and a CRC-32 of the 164 bytes before it."""
    boot_offset = UPDATE_HEADER
    system_offset = boot_offset + align(len(boot), 4096)
    fields = struct.pack("<8sII16s48sII32sII32sI", b"SOS2UPDT", 1, UPDATE_HEADER, BOARD,
                         validate_release(release), boot_offset, len(boot),
                         hashlib.sha256(boot).digest(), system_offset, len(system),
                         hashlib.sha256(system).digest(), system_offset + len(system))
    return (fields + struct.pack("<I", zlib.crc32(fields))).ljust(UPDATE_HEADER, b"\0")


def make_install_files(kernel, elf, dtb, ramdisk, release, display_format="native"):
    """The installed system from a RAM image's compressed resources: lk2nd's
    boot partition gets a boot image without a ramdisk, which names the
    resources' SHA-256 with sos2.system=, and the cache partition's first slot
    the resources. The update package carries both for the updater."""
    if display_format not in DISPLAY_CMDLINES:
        raise PackagingError("display format must be xrgb8888 or native")
    expanded = unpack_resources(ramdisk)
    files = {}
    for i in range(struct.unpack_from("<I", expanded, 12)[0]):
        name, offset, length = struct.unpack_from("<128sII", expanded, 16 + i * 136)
        files[name.split(b"\0", 1)[0]] = expanded[offset:offset + length]
    # The RAM image's resources, which keep it RAM-only; the installed boot
    # image's sos2.storage=installed gives the installed system its data
    # volume in the cache partition (fs.c, include/system_store.h).
    if files.get(b"/system/storage-mode") != b"RAM1":
        raise PackagingError("the installed system needs RAM-only resources")
    if files.get(b"/system/release") != validate_release(release):
        raise PackagingError("the resources must name the same release")
    payload, _ = boot_payload(kernel, elf, dtb)
    system_hash = hashlib.sha256(ramdisk).hexdigest()
    # relocate as for the RAM image: the kernel reads the slot to 0x82000000.
    cmdline = (DISPLAY_CMDLINES[display_format]
               + b",relocate sos2.storage=installed sos2.system=" + system_hash.encode("ascii"))
    boot = android_image(payload, cmdline, b"")
    if len(boot) > BOOT_IMAGE_LIMIT:
        raise PackagingError(f"the installed boot image exceeds {BOOT_IMAGE_LIMIT} bytes")
    slot = slot_header(ramdisk) + ramdisk
    cache = bytes(SLOT_A_OFFSET) + slot.ljust(align(len(slot), 4096), b"\0")
    update = update_header(release, boot, ramdisk) + boot.ljust(align(len(boot), 4096), b"\0") + ramdisk
    outputs = dict(zip(INSTALL_FILES, (boot, cache, update)))
    manifest = {
        "release": release,
        "board": BOARD.decode("ascii"),
        "boot_cmdline": cmdline.decode("ascii"),
        "system_sha256": system_hash,
        "wifi_profile": b"/system/wifi-profile" in files,
        "files": {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                  for name, data in outputs.items()},
        "flash": ["fastboot flash boot flash-boot.img", "fastboot flash cache flash-cache.img",
                  "fastboot reboot"],
    }
    return outputs, manifest


def write_outputs(outputs):
    """Stage the image and manifest together; roll back a failed publication.

    Unique staging names cannot overwrite an adjacent input or another build's
    temporary file. Each replacement is atomic, but the pair is not a
    crash-atomic transaction: consumers must still check the manifest's hash.
    """
    temp = Path(tempfile.mkdtemp(prefix=".sos2-package-", dir=outputs[0][0].parent))
    cleanup = True
    try:
        staged = []
        for index, (path, data) in enumerate(outputs):
            pending = temp / f"new-{index}"
            with pending.open("wb") as stream:
                os.fchmod(stream.fileno(),0o600)
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            backup = temp / f"old-{index}"
            if path.is_symlink() or path.exists():
                # Preserve the directory entry, including an existing symlink.
                shutil.copy2(path, backup, follow_symlinks=False)
            else:
                backup = None
            staged.append((path, pending, backup))
        published = []
        try:
            for path, pending, backup in staged:
                os.replace(pending, path)
                published.append((path, backup))
        except BaseException:
            for path, backup in reversed(published):
                try:
                    if backup is None:
                        path.unlink()
                    else:
                        os.replace(backup, path)
                except OSError as exc:
                    cleanup = False
                    raise PackagingError(f"Output recovery failed; backups retained in {temp}: {exc}") from exc
            raise
    finally:
        if cleanup:
            shutil.rmtree(temp)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path, help="Redmi 2 ARM32 kernel image")
    parser.add_argument("--elf", required=True, type=Path, help="matching Redmi 2 ELF (rejects QEMU images)")
    parser.add_argument("--dtb", required=True, type=Path, help="compiled boards/redmi2/boot.dts")
    parser.add_argument("--display-format", choices=tuple(DISPLAY_CMDLINES), default="xrgb8888",
                        help="request XRGB8888 conversion (default) or preserve lk2nd's native framebuffer format")
    parser.add_argument("--arch", choices=("arm32",), default="arm32",
                        help="kernel architecture (ARM32)")
    parser.add_argument("--output", required=True, type=Path, help="output boot image; also writes .json manifest")
    parser.add_argument('--resources',type=Path,help='native WebKit SORB bundle; delivered as a compressed ramdisk')
    parser.add_argument('--ram-only',action='store_true',help='temporary browser session; never initialize or access a disk')
    parser.add_argument('--wifi-firmware-dir',type=Path,help='local matching wcnss.mdt, split segments and board NV')
    parser.add_argument('--wifi-profile',type=Path,help='explicit Wi-Fi profile created by tools/redmi2_wifi.py')
    parser.add_argument('--adreno-firmware-dir',type=Path,help='pinned A306 firmware pair and redistribution notices')
    parser.add_argument('--rtc-anchor',help='explicit calibration UNIX_SECONDS:RTC_COUNTER, sampled together on this phone')
    parser.add_argument('--release',help='the build\'s name for the updater, 1-47 printable ASCII characters')
    parser.add_argument('--install-dir',type=Path,help='also write the installed system and update package here (RAM-only images)')
    parser.add_argument('--apps-dir',type=Path,help='write the apps the image leaves out here as packages, with index.txt')
    args = parser.parse_args(argv)
    manifest_path = args.output.with_suffix(args.output.suffix + ".json")
    install = [args.install_dir/name for name in (*INSTALL_FILES, 'install.json')] if args.install_dir else []
    install += [args.apps_dir/'index.txt'] if args.apps_dir else []
    try:
        if args.install_dir and not (args.ram_only and args.resources and args.release):
            raise PackagingError('the installed system needs --ram-only, --resources and --release')
        extra_files=None
        if args.wifi_profile and not args.wifi_firmware_dir:raise PackagingError('a bootstrap Wi-Fi profile requires matching firmware')
        if args.wifi_firmware_dir:
            from redmi2_wifi import resources
            try:extra_files=resources(args.wifi_firmware_dir,args.wifi_profile)
            except (ValueError,OSError) as error:raise PackagingError(str(error)) from error
        if args.adreno_firmware_dir:
            if not args.resources:raise PackagingError('Adreno firmware requires a resource bundle')
            from adreno_firmware import resources as adreno_resources
            try:extra_files={**(extra_files or {}),**adreno_resources(args.adreno_firmware_dir)}
            except (ValueError,OSError) as error:raise PackagingError(str(error)) from error
        rtc_anchor=None
        if args.rtc_anchor:
            try:
                rtc_anchor=tuple(int(x) for x in args.rtc_anchor.split(':'))
                if len(rtc_anchor)!=2:raise ValueError()
            except ValueError:raise PackagingError('RTC anchor must be UNIX_SECONDS:RTC_COUNTER')
        outputs_paths = (args.output, manifest_path, *install)
        if args.wifi_firmware_dir and any(out.resolve().is_relative_to(args.wifi_firmware_dir.resolve()) for out in outputs_paths):
            raise PackagingError('output must not overwrite the firmware backup')
        if args.adreno_firmware_dir and any(out.resolve().is_relative_to(args.adreno_firmware_dir.resolve()) for out in outputs_paths):
            raise PackagingError('output must not overwrite the Adreno firmware backup')
        inputs = (args.kernel, args.elf, args.dtb)
        all_inputs=inputs+((args.resources,) if args.resources else ())+((args.wifi_profile,) if args.wifi_profile else ())
        if any(out.resolve() == src.resolve() for out in outputs_paths for src in all_inputs):
            raise PackagingError("output must not overwrite an input file")
        kernel, elf, dtb = (path.read_bytes() for path in inputs)
        resources = args.resources.read_bytes() if args.resources else None
        if args.apps_dir and resources is None:raise PackagingError('app packages require a resource bundle')
        # The phone's images build in no apps: the updater is part of the system.
        image, manifest = make_boot_image(kernel, elf, dtb,
                                          display_format=args.display_format, arch=args.arch,
                                          resources=resources,rtc_anchor=rtc_anchor,extra_files=extra_files,ram_only=args.ram_only,
                                          release=args.release,strip_apps=resources is not None)
        outputs = [(args.output, image),
                   (manifest_path, (json.dumps(manifest, indent=2) + "\n").encode("utf-8"))]
        if args.install_dir:
            files, install_manifest = make_install_files(kernel, elf, dtb, android_ramdisk(image),
                                                         args.release, args.display_format)
            args.install_dir.mkdir(parents=True, exist_ok=True)
            outputs += [(args.install_dir/name, data) for name, data in files.items()]
            outputs.append((args.install_dir/'install.json',
                            (json.dumps(install_manifest, indent=2) + "\n").encode("utf-8")))
        packages = {}
        if args.apps_dir:
            packages = app_packages(bundle_files(resources))
            args.apps_dir.mkdir(parents=True, exist_ok=True)
            outputs += [(args.apps_dir/f'{app_id}.sos2app', package) for app_id, package in packages.items()]
            outputs.append((args.apps_dir/'index.txt', app_index(packages)))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        write_outputs(outputs)
        if args.apps_dir:
            # Packages of apps no longer in the bundle are not this build's.
            for stale in args.apps_dir.glob('*.sos2app'):
                if stale.stem not in packages:stale.unlink()
    except (OSError, PackagingError) as exc:
        print(f"Cannot package Redmi 2 image: {exc}", file=sys.stderr)
        return 1
    print(f"Created experimental image: {args.output} ({len(image)} bytes)")
    print(f"Manifest: {manifest_path}; no device was accessed.")
    if args.install_dir:
        print(f"Installed system and update: {args.install_dir} ({', '.join(INSTALL_FILES)})")
    if args.apps_dir:
        print(f"App packages: {args.apps_dir} ({', '.join(f'{app_id}.sos2app' for app_id in packages) or 'none'}, index.txt)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
