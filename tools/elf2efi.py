#!/usr/bin/env python3
"""Turn SOS2's UEFI loader, linked as a position-independent ELF
(boot/uefi/loader.ld), into a PE32+ EFI application: its .text, .rodata
and .data (with .bss) become PE sections at the same addresses, and its
relative relocations become PE base relocations, which the firmware
applies wherever it loads the image. The ELF may hold no other kind of
relocation; the pinned x86_64-elf binutils need no PE target for this."""
import argparse
from pathlib import Path
import struct
import sys

IMAGE_BASE = 0x140000000
SECTION_ALIGN = 0x1000
FILE_ALIGN = 0x200
R_X86_64_RELATIVE = 8
SHT_NOBITS = 8
SUBSYSTEM_EFI_APPLICATION = 10
# EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE; DYNAMIC_BASE | NX_COMPAT | HIGH_ENTROPY_VA.
FILE_FLAGS = 0x0022
DLL_FLAGS = 0x0160
CODE, READ_ONLY, WRITABLE, RELOCATIONS = 0x60000020, 0x40000040, 0xC0000040, 0x42000040


def align(value, alignment):
    return (value + alignment - 1) & -alignment


def elf_sections(elf):
    """The ELF's section headers by name: (type, address, offset, size)."""
    if elf[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<H', elf, 18)[0] != 62:
        raise ValueError('not an x64 ELF')
    shoff, = struct.unpack_from('<Q', elf, 40)
    size, count, names_index = struct.unpack_from('<HHH', elf, 58)
    headers = [struct.unpack_from('<IIQQQQIIQQ', elf, shoff + i * size) for i in range(count)]
    names = headers[names_index][4]

    def name(header):
        start = names + header[0]
        return elf[start:elf.index(b'\0', start)].decode()
    return {name(h): (h[1], h[3], h[4], h[5]) for h in headers}


def convert(elf):
    sections = elf_sections(elf)
    entry, = struct.unpack_from('<Q', elf, 24)
    loaded = [name for name in ('.text', '.rodata', '.data', '.bss') if name in sections and sections[name][3]]
    if '.text' not in loaded:
        raise ValueError('no code')
    end = max(sections[name][1] + sections[name][3] for name in loaded)
    image = bytearray(align(end, SECTION_ALIGN))
    for name in loaded:
        kind, address, offset, size = sections[name]
        if kind != SHT_NOBITS:
            image[address:address + size] = elf[offset:offset + size]
    relocations = []
    if '.rela.dyn' in sections:
        _, _, offset, size = sections['.rela.dyn']
        for at in range(offset, offset + size, 24):
            target, info, addend = struct.unpack_from('<QQq', elf, at)
            if info & 0xffffffff != R_X86_64_RELATIVE:
                raise ValueError(f'relocation type {info & 0xffffffff} at {target:#x}: only relative ones')
            struct.pack_into('<Q', image, target, IMAGE_BASE + addend)
            relocations.append(target)
    pe_sections = []

    def add(name, start, stop, raw_stop, flags):
        if stop > start:
            pe_sections.append((name, start, stop - start, bytes(image[start:raw_stop]), flags))
    text = sections['.text']
    add(b'.text', text[1], text[1] + text[3], text[1] + text[3], CODE)
    if '.rodata' in loaded:
        rodata = sections['.rodata']
        add(b'.rdata', rodata[1], rodata[1] + rodata[3], rodata[1] + rodata[3], READ_ONLY)
    writable = [sections[name] for name in ('.data', '.bss') if name in loaded]
    if writable:
        start = min(s[1] for s in writable)
        stop = max(s[1] + s[3] for s in writable)
        raw_stop = max([s[1] + s[3] for s in writable if s[0] != SHT_NOBITS] or [start])
        add(b'.data', start, stop, raw_stop, WRITABLE)
    for (_, a, a_size, _, _), (_, b, _, _, _) in zip(pe_sections, pe_sections[1:]):
        if a + a_size > b:
            raise ValueError('sections overlap')
    if any(a % SECTION_ALIGN for _, a, _, _, _ in pe_sections):
        raise ValueError('sections must start on page boundaries')
    # Base relocations: per 4 KiB page, 64-bit fixups (type 10), padded to a
    # 32-bit boundary; one empty block when there are none.
    blocks = bytearray()
    for page in sorted({r & ~0xfff for r in relocations}) or [0]:
        entries = [10 << 12 | r & 0xfff for r in sorted(relocations) if r & ~0xfff == page] or [0]
        if len(entries) % 2:
            entries.append(0)
        blocks += struct.pack(f'<II{len(entries)}H', page, 8 + 2 * len(entries), *entries)
    relocation_address = align(end, SECTION_ALIGN)
    pe_sections.append((b'.reloc', relocation_address, len(blocks), bytes(blocks), RELOCATIONS))
    image_size = align(relocation_address + len(blocks), SECTION_ALIGN)
    if not any(a <= entry < a + n for _, a, n, _, flags in pe_sections if flags == CODE):
        raise ValueError('the entry point is outside the code')

    count = len(pe_sections)
    headers_size = align(0x40 + 4 + 20 + 240 + 40 * count, FILE_ALIGN)
    if headers_size > pe_sections[0][1]:
        raise ValueError('no room for the PE headers before the first section')
    table, raw = bytearray(), bytearray()
    offset, code_size, data_size = headers_size, 0, 0
    for name, address, size, data, flags in pe_sections:
        raw_size = align(len(data), FILE_ALIGN)
        table += struct.pack('<8sIIIIIIHHI', name, size, address, raw_size, offset if raw_size else 0,
                             0, 0, 0, 0, flags)
        raw += data.ljust(raw_size, b'\0')
        offset += raw_size
        if flags == CODE:
            code_size += raw_size
        else:
            data_size += raw_size
    directories = [(0, 0)] * 16
    directories[5] = (relocation_address, len(blocks))
    optional = struct.pack('<HBBIIIIIQIIHHHHHHIIIIHHQQQQII', 0x20b, 0, 0, code_size, data_size, 0, entry,
                           pe_sections[0][1], IMAGE_BASE, SECTION_ALIGN, FILE_ALIGN, 0, 0, 0, 0, 0, 0, 0,
                           image_size, headers_size, 0, SUBSYSTEM_EFI_APPLICATION, DLL_FLAGS,
                           0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    optional += b''.join(struct.pack('<II', *d) for d in directories)
    coff = struct.pack('<HHIIIHH', 0x8664, count, 0, 0, 0, len(optional), FILE_FLAGS)
    dos = b'MZ'.ljust(0x3c, b'\0') + struct.pack('<I', 0x40)
    headers = (dos + b'PE\0\0' + coff + optional + table).ljust(headers_size, b'\0')
    return headers + raw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('efi', type=Path)
    args = parser.parse_args()
    try:
        efi = convert(args.elf.read_bytes())
    except (ValueError, IndexError, struct.error) as error:
        sys.exit(f'elf2efi: {args.elf}: {error}')
    partial = args.efi.with_name(args.efi.name + '.partial')
    partial.write_bytes(efi)
    partial.replace(args.efi)


if __name__ == '__main__':
    main()
