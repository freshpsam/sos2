#!/usr/bin/env python3
"""Pack read-only guest files for SOS2's bounded WebKit runtime."""
import argparse
from pathlib import Path
import struct


def bundle(output, files):
    if not 0 < len(files) <= 128:
        raise ValueError('bundle must contain 1..128 files')
    table = bytearray()
    payload = bytearray((16 + 136 * len(files) + 4095) & ~4095)
    for name, source in sorted(files.items()):
        encoded = name.encode()
        if not name.startswith('/') or len(encoded) >= 128 or b'\0' in encoded:
            raise ValueError(f'invalid guest path: {name}')
        data = source.read_bytes() if isinstance(source, Path) else source
        table += struct.pack('<128sII', encoded, len(payload), len(data))
        payload += data
        payload += bytes(-len(payload) % 4096)
    if len(payload) > 0x10000000:
        raise ValueError('resource bundle exceeds reserved memory')
    payload[:16] = struct.pack('<IIII', 0x42524f53, 1, len(payload), len(files))  # b'SORB'
    payload[16:16+len(table)] = table
    output.parent.mkdir(parents=True, exist_ok=True)
    # Replace the file whole: a QEMU or packager run never reads a partial one.
    partial = output.with_name(output.name+'.partial')
    partial.write_bytes(payload)
    partial.replace(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('files', nargs='+', help='absolute guest path=local source')
    args = parser.parse_args()
    files = {}
    for spec in args.files:
        name, source = spec.split('=', 1)
        if name in files:
            parser.error(f'duplicate path: {name}')
        files[name] = Path(source)
    bundle(args.output, files)


if __name__ == '__main__':
    main()
