#!/usr/bin/env python3
"""Build lk2nd boot images and the installed system; never access a device.

The source-backed contract and device prerequisites are in docs/redmi2.md.
Only a verified Redmi 2 ELF/raw pair and a minimal wt88047 DTB are accepted.
With --install-dir, the same image, its data kept on the eMMC, also becomes
the installed system: flash-boot.img for lk2nd's boot partition, and
sos2-redmi2.update, which the system's updater, /bin/sos2-updater, downloads
(docs/redmi2.md#installing). The phone's
images build in no apps; with --apps-dir, the bundle's apps become packages,
<id>.sos2app, with index.txt, which the updater installs one by one
(docs/redmi2.md#apps).
"""

import argparse
import array
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
# SORZ v3 resources: a 128-byte header and one raw LZMA stream with an end
# marker, which the kernel decodes with third_party/lzma
# (kernel/protection/boot_bundle.c). The header carries the LZMA properties
# byte; the kernel accepts lc+lp <= 3 and sizes the dictionary to the whole
# output. lp=1 suits the engine's 2- and 4-byte instructions: LZMA's default
# lc=3, lp=0 makes the resources about 0.5% larger.
# The engine's code goes through branch filters first, which make each call's
# target absolute, so that calls to one function repeat: xz's ARM64 filter
# for an AArch64 engine, about 7% smaller compressed, and for an ARM32
# engine, mostly Thumb-2 with ARM libraries, an ARM and a Thumb-2 filter over
# each instruction set's ranges, about 9%. The header names up to
# SORZ_RANGES filters with their ranges of the expanded data, which the
# kernel converts back.
LZMA_FILTERS = [{'id': lzma.FILTER_LZMA1, 'preset': 9 | lzma.PRESET_EXTREME,
                 'dict_size': 64 * 1024 * 1024, 'lc': 2, 'lp': 1, 'pb': 2}]
LZMA_PROPERTIES = (LZMA_FILTERS[0]['pb'] * 5 + LZMA_FILTERS[0]['lp']) * 9 + LZMA_FILTERS[0]['lc']
SORZ_MAGIC = 0x5a524f53
SORZ_VERSION = 3
SORZ_HEADER = 128
SORZ_RANGES = 8
SORZ_FILTER_NONE = 0
SORZ_FILTER_ARM64 = 1
SORZ_FILTER_ARM = 2
SORZ_FILTER_THUMB = 3
MAX_DTB_SIZE = 1024 * 1024
# What lk2nd leaves of the 32 MiB boot partition after its own 512 KiB: a
# boot image with everything, the engine included, must fit there too.
BOOT_PARTITION_SPACE = 32 * 1024 * 1024 - 512 * 1024
# lk2nd passes its framebuffer as it is, in the panel's native format.
CMDLINE = b"lk2nd.pass-simplefb=autorefresh"
# The installed system (include/system_store.h, kernel/protection/system_update.c):
# lk2nd keeps its first 512 KiB of the 32 MiB boot partition and boots the
# image after it, which holds everything, as the RAM image does; the cache
# partition holds the app store and the data volume, which SOS2 formats. An
# update is that boot image after a 4 KiB header.
UPDATE_HEADER = 4096
UPDATE_VERSION = 2
BOARD = b"redmi2-wt88047"
RELEASE_MAX = 47
INSTALL_FILES = ("flash-boot.img", "sos2-redmi2.update")
# App packages (struct app_package in include/app_store.h): a 512-byte header,
# the 48x48 ARGB icon and the program, at most one app store half less its
# 4 KiB header.
APP_PACKAGE_HEADER = 512
APP_ICON_BYTES = 48 * 48 * 4
APP_PACKAGE_MAX = 6144 * 512 - 4096
APP_INDEX = b"SOS2 apps 1"
USER_ABI = (Path(__file__).resolve().parents[1] / "include/user_abi.h").read_text()
ABI = int(re.search(r"#define USER_ABI_VERSION (\d+)", USER_ABI).group(1))
# AArch64's carries EM_AARCH64 (183) above its version, and x64's EM_X86_64
# (62), so none of them match.
ABI_ARM64 = 183 << 16 | int(re.search(r"#define USER_ABI_VERSION \(\(183u << 16\) \| (\d+)\)", USER_ABI).group(1))
ABI_X64 = 62 << 16 | int(re.search(r"#define USER_ABI_VERSION \(\(62u << 16\) \| (\d+)\)", USER_ABI).group(1))


class PackagingError(ValueError):
    """Invalid or incompatible input; no image should be produced."""


def align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def validate_arm32_kernel(kernel, elf, base=ARM32_KERNEL_ADDRESS, limit=TAGS_ADDRESS, device="Redmi 2"):
    """Validate a flat ARM-state image with the zImage header used to locate its DTB.

    The image is the ELF's file contents linked at base, and its memory ends
    below limit, where the loader copies the DTB. lk2nd 23.1 forces
    DDR_START+0x8000 for ARM32 and reads the appended DTB offset at 0x2c
    (platform/msm8916/include/platform/iomap.h and
    platform/msm_shared/dev_tree.c); the Nexus 5's bootloader, also LK, the
    same at 0x8000. The payload has no decompressor. Returns the kernel's
    allocation in whole pages.
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
    if (elf_type, machine, version, entry, ehsize) != (2, 40, 1, base, 52):
        raise PackagingError(f"ELF is not an ARM executable linked for the {device} at 0x{base:08x}")
    if flags & 0xFF000000 != 0x05000000 or flags & 0x400:
        raise PackagingError("ARM32 ELF must use EABI5 without the hard-float calling convention")
    if phentsize != 32 or not 1 <= phnum <= 64 or phoff + phentsize * phnum > len(elf):
        raise PackagingError("invalid ELF program header table")
    expected = bytearray(len(kernel))
    written = bytearray(len(kernel))
    first, file_end, memory_end = None, 0, base
    for index in range(phnum):
        kind, offset, vaddr, paddr, filesz, memsz, _, _ = struct.unpack_from(
            "<8I", elf, phoff + index * phentsize
        )
        if kind != 1 or memsz == 0:
            continue
        if (vaddr != paddr or paddr < base or filesz > memsz
                or paddr + memsz > limit or offset + filesz > len(elf)):
            raise PackagingError("ARM32 ELF load segment is outside the kernel's reserved allocation")
        memory_end = max(memory_end, paddr + memsz)
        if not filesz:
            continue
        start = paddr - base
        end = start + filesz
        if end > len(kernel) or any(written[start:end]):
            raise PackagingError("ELF file segments overlap or exceed the raw image")
        expected[start:end] = elf[offset:offset + filesz]
        written[start:end] = b"\x01" * filesz
        first = paddr if first is None else min(first, paddr)
        file_end = max(file_end, end)
    if first != base or file_end != len(kernel) or expected != kernel:
        raise PackagingError(f"raw kernel does not match the supplied {device} ELF")
    return align(memory_end - base, PAGE_SIZE)


def parse_dtb(data, *, max_size=MAX_DTB_SIZE):
    """Parse bounded FDT v17 data, rejecting ambiguous nodes/properties/trailers."""
    if len(data) < 40 or len(data) > max_size:
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


def arm64_branches(data,position,encode):
    """xz's ARM64 branch filter (liblzma/simple/arm64.c) over whole words:
    BL's and ADRP's relative targets become absolute from each word's
    position in the expanded data when encoding, and relative again when
    decoding."""
    words=array.array('I');words.frombytes(data[:len(data)//4*4])
    if sys.byteorder!='little':words.byteswap()
    sign=1 if encode else -1
    for i,word in enumerate(words):
        if word>>26==0x25:
            words[i]=0x94000000|((word+sign*((position+4*i)>>2))&0x03ffffff)
        elif word&0x9f000000==0x90000000:
            source=(word>>29)&3|(word>>3)&0x001ffffc
            if (source+0x00020000)&0x001c0000:continue
            target=(source+sign*((position+4*i)>>12))&0xffffffff
            words[i]=word&0x9000001f|(target&3)<<29|(target&0x0003fffc)<<3|(-(target&0x00020000))&0x00e00000
    if sys.byteorder!='little':words.byteswap()
    return words.tobytes()+data[len(data)//4*4:]


def arm_branches(data,position,encode):
    """ARM's B and BL with the always condition, and BLX, over whole words, as
    arm64_branches does AArch64's: B's and BL's targets count words, BLX's, a
    Thumb function's, halfwords, with its H bit the lowest."""
    out=bytearray(data);sign=1 if encode else -1
    for match in re.finditer(rb'[\xea\xeb\xfa\xfb]',data[3:len(data)//4*4:4]):
        i=match.start()*4;word,=struct.unpack_from('<I',out,i);top=word>>24;pc=position+i+8
        if top<0xfa:word=top<<24|(word+sign*(pc>>2))&0xffffff
        else:
            target=((word&0xffffff)<<1|top&1)+sign*(pc>>1)
            word=(0xfa|target&1)<<24|target>>1&0xffffff
        struct.pack_into('<I',out,i,word)
    return bytes(out)


# A walk over Thumb-2 instructions up to the next BL, BLX or B.W: a halfword
# from 0xe800 up starts a 32-bit instruction, one from 0xf000 to 0xf7ff a
# branch if its second halfword says so (BLX with its H bit clear).
_EVEN=b'['+b''.join(re.escape(bytes([byte])) for byte in range(0,256,2))+b']'
_ODD=b'['+b''.join(re.escape(bytes([byte])) for byte in range(1,256,2))+b']'
THUMB_BRANCH=re.compile(
    rb'(?:.[\x00-\xe7]|.[\xe8-\xef\xf8-\xff]..|.[\xf0-\xf7](?:.[\x00-\x8f\xa0-\xaf]|'+_ODD+rb'[\xc0-\xcf\xe0-\xef]))*+'
    rb'.[\xf0-\xf7](?:.[\x90-\x9f\xb0-\xbf\xd0-\xdf\xf0-\xff]|'+_EVEN+rb'[\xc0-\xcf\xe0-\xef])',re.DOTALL)


def thumb_branches(data,position,encode):
    """Thumb-2's BL, BLX and B.W, as arm_branches does ARM's: their 24-bit
    fields hold the target in halfwords, with S, J1 and J2 as its top bits.
    BLX's target counts from the word-aligned PC, as the instruction does. The
    walk sees the same instructions both ways: filtering keeps the bits it
    reads."""
    out=bytearray(data);i=0
    while match:=THUMB_BRANCH.match(data,i):
        i=match.end();first,second=struct.unpack_from('<2H',data,i-4)
        pc=position+i if second&0xd000!=0xc000 else position+i&~3
        if encode:
            s=first>>10&1
            offset=s<<23|(~(second>>13^s)&1)<<22|(~(second>>11^s)&1)<<21|(first&0x3ff)<<11|second&0x7ff
            target=offset+(pc>>1)&0xffffff
            first=first&0xf800|target>>13
            second=second&0xd000|(target>>12&1)<<13|(target>>11&1)<<11|target&0x7ff
        else:
            target=(first&0x7ff)<<13|(second>>13&1)<<12|second&0xfff
            offset=target-(pc>>1)&0xffffff;s=offset>>23
            first=first&0xf800|s<<10|offset>>11&0x3ff
            second=second&0xd000|(~(offset>>22^s)&1)<<13|(~(offset>>21^s)&1)<<11|offset&0x7ff
        struct.pack_into('<2H',out,i-4,first,second)
    return bytes(out)


BRANCH_FILTERS={SORZ_FILTER_ARM64:arm64_branches,SORZ_FILTER_ARM:arm_branches,SORZ_FILTER_THUMB:thumb_branches}


def arm32_ranges(program,offset):
    """An ARM32 engine's code by instruction set, as filter ranges of the
    expanded data, where the program starts at offset: its executable segment
    in 4 KiB blocks, ARM where at least 40% of the words carry ARM's always
    condition, Thumb-2 elsewhere, the shortest stretches given to their
    neighbours until SORZ_RANGES remain. Every choice decodes; a poor one only
    compresses worse."""
    phoff,=struct.unpack_from('<I',program,28);size,count=struct.unpack_from('<2H',program,42)
    if size!=32 or phoff+32*count>len(program):return []
    code=[(start,start+length) for kind,start,_,_,length,_,flags,_ in
          (struct.unpack_from('<8I',program,phoff+32*i) for i in range(count))
          if kind==1 and flags&1 and length and start+length<=len(program)]
    if len(code)!=1 or code[0][0]%4:return []
    start,end=code[0][0],code[0][1]//4*4
    condition=program[start+3:end:4].translate(bytes(0xe0<=byte<=0xef for byte in range(256)))
    runs=[]
    for block in range(start,end,4096):
        words=condition[(block-start)//4:(block-start)//4+1024]
        kind=SORZ_FILTER_ARM if 10*sum(words)>=4*len(words) else SORZ_FILTER_THUMB
        if runs and runs[-1][0]==kind:runs[-1][2]=min(end,block+4096)
        else:runs.append([kind,block,min(end,block+4096)])
    while len(runs)>SORZ_RANGES:
        i=min(range(len(runs)),key=lambda i:runs[i][2]-runs[i][1])
        if i==0:runs[1][1]=runs[0][1];del runs[0]
        elif i==len(runs)-1:runs[i-1][2]=runs[i][2];del runs[i]
        else:runs[i-1][2]=runs[i+1][2];del runs[i:i+2]
    return [(kind,offset+first,last-first) for kind,first,last in runs]


def branch_filters(data):
    """The branch filters for a SORB, as (filter, start, length): its engine's
    code, by instruction set."""
    for i in range(struct.unpack_from('<I',data,12)[0]):
        name,offset,size=struct.unpack_from('<128sII',data,16+i*136)
        program=data[offset:offset+size]
        if name.split(b'\0',1)[0]!=b'/bin/sos2-web' or size<52:continue
        machine,=struct.unpack_from('<H',program,18)
        if program[:7]==b'\x7fELF\x02\x01\x01' and machine==183:return [(SORZ_FILTER_ARM64,offset,size//4*4)]
        if program[:7]==b'\x7fELF\x01\x01\x01' and machine==40:return arm32_ranges(program,offset)
    return []


def filters_valid(filters,expanded):
    """Branch filters as the kernel accepts them: known ones over aligned,
    ordered, separate ranges of the expanded data, then unused ones, zero."""
    end=0;unused=False
    for kind,start,length in filters:
        if not kind:
            if start or length:return False
            unused=True;continue
        align=2 if kind==SORZ_FILTER_THUMB else 4
        if (unused or kind not in BRANCH_FILTERS or not length or start%align or length%align or
                start<end or start+length>expanded):return False
        end=start+length
    return True


def apply_filters(data,filters,encode):
    """data with each branch filter applied to its range."""
    out=bytearray(data)
    for kind,start,length in filters:
        if kind:out[start:start+length]=BRANCH_FILTERS[kind](bytes(out[start:start+length]),start,encode)
    return bytes(out)


def unpack_resources(packed):
    """The expanded SORB of a SORZ v3 ramdisk, decoded as the kernel does: with
    the header's properties byte, sizes and branch filter, checked against its
    CRC-32s."""
    if len(packed)<=SORZ_HEADER:raise PackagingError('truncated SORZ ramdisk')
    magic,version,header,size,expanded,crc,expanded_crc,properties=struct.unpack_from('<8I',packed)
    ranges=struct.unpack_from(f'<{3*SORZ_RANGES}I',packed,32)
    filters=[ranges[i:i+3] for i in range(0,len(ranges),3)]
    lc,lp,pb=properties%9,properties//9%5,properties//45
    if ((magic,version,header,size)!=(SORZ_MAGIC,SORZ_VERSION,SORZ_HEADER,len(packed)-SORZ_HEADER) or
            not 16<=expanded<=256*1024*1024 or pb>4 or lc+lp>3 or not filters_valid(filters,expanded) or
            zlib.crc32(packed[SORZ_HEADER:])!=crc):
        raise PackagingError('invalid SORZ v3 ramdisk header')
    try:
        data=lzma.decompress(packed[SORZ_HEADER:],format=lzma.FORMAT_RAW,
                             filters=[{'id':lzma.FILTER_LZMA1,'dict_size':expanded,'lc':lc,'lp':lp,'pb':pb}])
    except lzma.LZMAError as error:
        raise PackagingError('SORZ ramdisk does not decode: '+str(error)) from error
    if len(data)==expanded:data=apply_filters(data,filters,False)
    if len(data)!=expanded or zlib.crc32(data)!=expanded_crc:
        raise PackagingError('SORZ ramdisk does not decode to its recorded size and CRC-32')
    return data


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


# Programs by architecture: ELF class, ARM's (40), AArch64's (183) or x64's
# (62) machine.
ELF_ARM32 = (1, 40)
ELF_ARM64 = (2, 183)
ELF_X64 = (2, 62)
ELF_NAMES = {ELF_ARM32: "ARM32", ELF_ARM64: "AArch64", ELF_X64: "x64"}


def app_package(app_id, name, icon, program, abi=None, elf=ELF_ARM32):
    """An app's .sos2app: header, icon and program (include/app_store.h),
    whose program is for elf's architecture, built for its ABI by default."""
    if abi is None:
        abi = {ELF_ARM64: ABI_ARM64, ELF_X64: ABI_X64}.get(elf, ABI)
    if not re.fullmatch(r"[a-z0-9-]{1,15}", app_id):
        raise PackagingError(f"app id {app_id!r} is not 1-15 lowercase letters, digits and hyphens")
    if not 1 <= len(name) <= 12 or not all(32 <= c < 127 for c in name):
        raise PackagingError(f"app {app_id}: the name must be 1-12 printable ASCII characters")
    if (len(icon) != APP_ICON_BYTES or len(program) < 64 or program[:4] != b"\x7fELF" or
            program[4:7] != bytes((elf[0], 1, 1)) or struct.unpack_from("<H", program, 18)[0] != elf[1]):
        raise PackagingError(f"app {app_id}: needs a 48x48 icon and an {ELF_NAMES[elf]} ELF program")
    payload = icon + program
    fields = struct.pack("<8sII16s16sIIIII32s", b"SOS2APP1", 1, abi, app_id.encode("ascii"), name,
                         APP_PACKAGE_HEADER, len(icon), APP_PACKAGE_HEADER + len(icon), len(program),
                         APP_PACKAGE_HEADER + len(payload), hashlib.sha256(payload).digest())
    package = (fields + struct.pack("<I", zlib.crc32(fields))).ljust(APP_PACKAGE_HEADER, b"\0") + payload
    if len(package) > APP_PACKAGE_MAX:
        raise PackagingError(f"app {app_id} needs {len(package)} bytes; a package holds {APP_PACKAGE_MAX}")
    return package


def app_packages(files, elf=ELF_ARM32):
    """The bundle's apps, as packages by id."""
    return {app_id: app_package(app_id, files[f"/apps/{app_id}/name".encode()],
                                files[f"/apps/{app_id}/icon".encode()],
                                files[f"/apps/{app_id}/program".encode()], elf=elf)
            for app_id in app_ids(files)}


def app_index(packages):
    """index.txt: a line naming the format, then one per app: id, ABI, size,
    SHA-256 and name, as the updater reads them."""
    lines = [APP_INDEX]
    for app_id, package in sorted(packages.items()):
        name = package[32:48].split(b"\0", 1)[0]
        abi = struct.unpack_from("<I", package, 12)[0]
        lines.append(b"%s %d %d %s %s" % (app_id.encode("ascii"), abi, len(package),
                                          hashlib.sha256(package).hexdigest().encode("ascii"), name))
    return b"\n".join(lines) + b"\n"


def resource_bundle(data,extra_files=None,ram_only=False,release=None,strip_apps=False):
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
    if extra_files or ram_only or release is not None or stripped:
        data=sorb(files)
    if len(data)>256*1024*1024:
        raise PackagingError('expanded native resources exceed the kernel limit')
    return data


def code_offset(program):
    """The file offset of an ARM32 executable's one read-only segment, or None."""
    if len(program)<52 or program[:7]!=b'\x7fELF\x01\x01\x01':return None
    phoff,=struct.unpack_from('<I',program,28);size,count=struct.unpack_from('<HH',program,42)
    if size!=32 or phoff+count*size>len(program):return None
    headers=[struct.unpack_from('<8I',program,phoff+i*size) for i in range(count)]
    fixed=[h[1] for h in headers if h[0]==1 and not h[6]&2]
    return fixed[0] if len(fixed)==1 else None


def sorb(files):
    """A SORB of these files, each page-aligned after the table, as the kernel
    reads it (kernel/protection/runtime_files.c), and the engine's code on a
    megabyte, as the phones' kernels place the whole (phone_resource_arena),
    so that pages map it in place (kernel/protection/elf_load.c)."""
    if len(files)>128:raise PackagingError('no room for board resources in the resource table')
    table=bytearray();payload=bytearray();offset=align(16+136*len(files),4096)
    for name,content in files.items():
        code=code_offset(content) if name==b'/bin/sos2-web' else None
        if code is not None:payload+=bytes(-(offset+len(payload)+code)%0x100000)
        table+=struct.pack('<128sII',name,offset+len(payload),len(content))
        payload+=content;payload+=bytes(align(len(payload),4096)-len(payload))
    return (struct.pack('<4I',0x42524f53,1,offset+len(payload),len(files))+table).ljust(offset,b'\0')+payload


def sorz(data):
    """The SORZ v3 ramdisk for an expanded SORB, checked by decoding it again;
    the engine's code goes through the branch filters."""
    filters=branch_filters(data)
    if not filters_valid(filters,len(data)) or len(filters)>SORZ_RANGES:
        raise PackagingError('invalid branch filter ranges for the engine')
    compressed=lzma.compress(apply_filters(data,filters,True),format=lzma.FORMAT_RAW,filters=LZMA_FILTERS)
    ranges=[value for branch_filter in filters for value in branch_filter]
    header=struct.pack(f'<8I{3*SORZ_RANGES}I',SORZ_MAGIC,SORZ_VERSION,SORZ_HEADER,len(compressed),len(data),
                       zlib.crc32(compressed),zlib.crc32(data),LZMA_PROPERTIES,
                       *ranges,*[0]*(3*SORZ_RANGES-len(ranges)))
    packed=header+compressed
    if unpack_resources(packed)!=data:
        raise PackagingError('compressed resources do not decode to the bundle')
    return packed


def compress_resources(data):
    """sorz(data), within lk2nd's ramdisk window."""
    packed=sorz(data)
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


def android_image(payload, cmdline, ramdisk, kernel_address=ARM32_KERNEL_ADDRESS,
                  ramdisk_address=RAMDISK_ADDRESS, second_address=0x80F00000,
                  tags_address=TAGS_ADDRESS, name=b"sos2-redmi2"):
    """An Android boot v0 image of 2 KiB pages: header, payload, ramdisk,
    with the addresses the loader puts them at (the Redmi 2's by default)."""
    if len(cmdline) >= 512:
        raise PackagingError("boot command line exceeds the Android header's 512 bytes")
    digest = hashlib.sha1()
    digest.update(payload)
    digest.update(struct.pack("<I", len(payload)))
    digest.update(ramdisk)
    digest.update(struct.pack("<I", len(ramdisk)))
    digest.update(struct.pack("<I", 0))  # Empty second-stage payload.
    header = struct.pack(
        "<8s10I16s512s32s1024s", b"ANDROID!", len(payload), kernel_address,
        len(ramdisk), ramdisk_address, 0, second_address, tags_address, PAGE_SIZE, 0, 0,
        name, cmdline, digest.digest(), b"",
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


def with_jsc_options(cmdline, options):
    if options is None:
        return cmdline
    if not options or len(options) > 127 or not all('!' <= c <= '~' for c in options):
        raise PackagingError('JavaScriptCore options are 1-127 printable ASCII characters without spaces')
    prefix = cmdline + b' sos2.jsc='
    # The kernel keeps 255 bytes of sos2.* arguments from /chosen/bootargs,
    # and installed updaters check that the image's whole line fits in them.
    available = 255 - len(prefix)
    if len(options) > available:
        raise PackagingError(f'JavaScriptCore options exceed the {available} bytes left in the boot command line')
    return prefix + options.encode('ascii')


def make_boot_image(kernel, elf, dtb, resources=None,extra_files=None,ram_only=False,release=None,strip_apps=False,jsc=None):
    cmdline = CMDLINE
    if resources is not None:
        # MSM8916 forces ramdisks to 0x82000000; the full browser crosses
        # MIPI_FB_ADDR (0x83200000). Ask lk2nd to move scanout before handing
        # control to us. Its simplefb hook relocates to scratch + 256 - 10 MiB,
        # publishes/reserves the new address, and leaves the ramdisk intact.
        # Merely changing the Android header's ramdisk address cannot fix this:
        # lk2nd overwrites that address. SOS2 still validates the actual DTB.
        cmdline += b",relocate"
    if ram_only:
        if resources is None:
            raise PackagingError('RAM-only mode requires the protected ARM32 browser bundle')
        cmdline+=b' sos2.storage=ram'
    cmdline = with_jsc_options(cmdline, jsc)
    payload, image_size = boot_payload(kernel, elf, dtb)
    if extra_files and resources is None:raise PackagingError('Wi-Fi provisioning requires native browser resources')
    if release is not None and resources is None:raise PackagingError('a release name requires a resource bundle')
    expanded=resource_bundle(resources,extra_files,ram_only,release,strip_apps) if resources is not None else None
    ramdisk=compress_resources(expanded) if expanded is not None else b''
    kernel_address = ARM32_KERNEL_ADDRESS
    image_format = "Android boot v0; uncompressed ARM32 zImage-shaped image followed by one DTB"
    entry_requirement = "32-bit ARM-state entry with device tree in r2"
    image = android_image(payload, cmdline, ramdisk)
    if len(image) > BOOT_PARTITION_SPACE:
        raise PackagingError(f'boot.img needs {len(image)} bytes; the boot partition holds '
                             f'{BOOT_PARTITION_SPACE} after lk2nd')
    manifest = {
        "status": "built",
        "target": "Redmi 2 wt88047 / lk2nd-msm8916",
        "lk2nd_source_revision": LK2ND_REVISION,
        "architecture": "arm32",
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
        manifest['resources']={'format':'SORZ v3 / raw LZMA / CRC32',
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
        manifest['requirements']+=['protected kernel with SORZ decoding',
            'lk2nd continuous-splash framebuffer relocation support',
            'reported RAM and live framebuffer must not overlap the ramdisk or expanded resources']
    if release is not None:manifest['release']=release
    if resources is not None:
        manifest['resources']['apps']=app_ids(bundle_files(expanded))
        if strip_apps:manifest['resources']['apps_left_out']=app_ids(bundle_files(resources))
    return image, manifest


def update_header(release, boot, board=BOARD):
    """sos2-redmi2.update's first 4 KiB (struct system_update_header in
    include/system_store.h): the release and board, where the boot image
    follows with its size and SHA-256, the file's size and a CRC-32 of the 124
    bytes before it."""
    fields = struct.pack("<8sII16s48sII32sI", b"SOS2UPDT", UPDATE_VERSION, UPDATE_HEADER, board,
                         validate_release(release), UPDATE_HEADER, len(boot),
                         hashlib.sha256(boot).digest(), UPDATE_HEADER + len(boot))
    return (fields + struct.pack("<I", zlib.crc32(fields))).ljust(UPDATE_HEADER, b"\0")


def install_outputs(release, board, names, boot, profile, cmdline=None, update_image=None, *, wifi_firmware):
    """What tools/publish_pages.py publishes of a device's installed system:
    names' boot image, the updater's file (update_header, then update_image,
    the boot image unless given) and install.json, which lists both with
    their sizes and SHA-256s, the boot command line when given, whether the
    Wi-Fi firmware and a Wi-Fi profile are inside, and the commands of a
    first install from the loader's fastboot."""
    image = boot if update_image is None else update_image
    outputs = dict(zip(names, (boot, update_header(release, image, board) + image)))
    manifest = {"release": release, "board": board.decode("ascii")}
    if cmdline is not None:
        manifest["boot_cmdline"] = cmdline.decode("ascii")
    manifest["wifi_firmware"] = wifi_firmware
    manifest["wifi_profile"] = profile
    manifest["files"] = {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                         for name, data in outputs.items()}
    manifest["flash"] = ["fastboot flash boot flash-boot.img", "fastboot reboot"]
    outputs["install.json"] = (json.dumps(manifest, indent=2) + "\n").encode("utf-8")
    return outputs


def make_install_files(kernel, elf, dtb, ramdisk, release, jsc=None):
    """The installed system from a RAM image's compressed resources: lk2nd's
    boot partition gets the same boot image with sos2.storage=installed, which
    keeps data in the cache partition. The update package carries it for the
    updater."""
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
    # relocate as for the RAM image: lk2nd loads the ramdisk to 0x82000000.
    cmdline = CMDLINE + b",relocate sos2.storage=installed"
    cmdline = with_jsc_options(cmdline, jsc)
    boot = android_image(payload, cmdline, ramdisk)
    if len(boot) > BOOT_PARTITION_SPACE:
        raise PackagingError(f"flash-boot.img needs {len(boot)} bytes; the boot partition holds "
                             f"{BOOT_PARTITION_SPACE} after lk2nd")
    return install_outputs(release, BOARD, INSTALL_FILES, boot, b"/system/wifi-profile" in files, cmdline,
                           wifi_firmware=any(name.startswith(b"/firmware/wcnss/") for name in files))


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
    parser.add_argument("--output", required=True, type=Path, help="output boot image; also writes .json manifest")
    parser.add_argument('--resources',type=Path,help='native WebKit SORB bundle; delivered as a compressed ramdisk')
    parser.add_argument('--ram-only',action='store_true',help='temporary browser session; never initialize or access a disk')
    parser.add_argument('--wifi-firmware-dir',type=Path,help='local matching wcnss.mdt, split segments and board NV')
    parser.add_argument('--wifi-profile',type=Path,help='explicit Wi-Fi profile created by tools/redmi2_wifi.py')
    parser.add_argument('--adreno-firmware-dir',type=Path,help='pinned A306 firmware pair and redistribution notices')
    parser.add_argument('--release',help='the build\'s name for the updater, 1-47 printable ASCII characters')
    parser.add_argument('--jsc',help='JavaScriptCore options for pages, comma-separated (the sos2.jsc= boot argument)')
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
                                          resources=resources,extra_files=extra_files,ram_only=args.ram_only,
                                          release=args.release,strip_apps=resources is not None,jsc=args.jsc)
        outputs = [(args.output, image),
                   (manifest_path, (json.dumps(manifest, indent=2) + "\n").encode("utf-8"))]
        if args.install_dir:
            files = make_install_files(kernel, elf, dtb, android_ramdisk(image), args.release, args.jsc)
            args.install_dir.mkdir(parents=True, exist_ok=True)
            outputs += [(args.install_dir/name, data) for name, data in files.items()]
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
    print(f"Created boot image: {args.output} ({len(image)} bytes)")
    print(f"Manifest: {manifest_path}; no device was accessed.")
    if args.install_dir:
        print(f"Installed system and update: {args.install_dir} ({', '.join(INSTALL_FILES)})")
    if args.apps_dir:
        print(f"App packages: {args.apps_dir} ({', '.join(f'{app_id}.sos2app' for app_id in packages) or 'none'}, index.txt)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
