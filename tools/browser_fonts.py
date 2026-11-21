#!/usr/bin/env python3
"""Fetch pinned browser fonts explicitly; package only verified local assets.

An asset with a "subset" is packaged cut down to that character set, by
tools/font_subset.cc built from the pinned HarfBuzz source the browser links:
"latin" keeps English text and the symbols around it, "no-flags" a font's own
characters but the regional indicators and tags that spell flags, which take
a third of the emoji font. SOS2's language is English alone, so that the
whole system fits the Nexus 5's 22 MiB boot partition (docs/nexus5.md)."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
from native_sources import digest, download

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / 'build/native-webkit/fonts'
MANIFEST = ROOT / 'tools/browser/fonts.lock.json'
SUBSETTER = ROOT / 'build/host/font-subset'


def records():
    return json.loads(MANIFEST.read_text())['assets']


def valid(path, record):
    if not path.is_file() or path.stat().st_size != record['bytes']:
        return False
    return hashlib.sha256(path.read_bytes()).hexdigest() == record['sha256']


def fetch():
    for record in records():
        download(record['url'], CACHE / record['file'], record['sha256'])


# English text: Latin with Latin-1 and Extended-A, the spacing modifiers and
# combining marks, and Greek for its letters in formulas; then punctuation,
# super- and subscripts, currency, letterlike symbols, number forms, arrows,
# mathematical operators, technical symbols, enclosed alphanumerics, box
# drawing, blocks, shapes, miscellaneous symbols, dingbats, the Latin
# ligatures and the replacement character.
LATIN = ((0x20, 0x7e), (0xa0, 0x17f), (0x2b0, 0x3ff), (0x2000, 0x23ff), (0x2460, 0x27bf),
         (0xfb00, 0xfb06), (0xfffd, 0xfffd))


def characters(path):
    """The characters a font maps, from its cmap's Unicode subtables (formats
    4 and 12)."""
    font = path.read_bytes()
    tables = {font[12 + 16 * i:16 + 16 * i]: struct.unpack_from('>I', font, 20 + 16 * i)[0]
              for i in range(struct.unpack_from('>H', font, 4)[0])}
    cmap, points = tables[b'cmap'], set()
    for i in range(struct.unpack_from('>H', font, cmap + 2)[0]):
        platform, _, offset = struct.unpack_from('>HHI', font, cmap + 4 + 8 * i)
        table = cmap + offset
        kind = struct.unpack_from('>H', font, table)[0]
        if platform not in (0, 3):
            continue
        if kind == 12:
            for group in range(struct.unpack_from('>I', font, table + 12)[0]):
                first, last = struct.unpack_from('>II', font, table + 16 + 12 * group)
                points.update(range(first, last + 1))
        elif kind == 4:
            segments = struct.unpack_from('>H', font, table + 6)[0] // 2
            for segment in range(segments):
                last = struct.unpack_from('>H', font, table + 14 + 2 * segment)[0]
                first = struct.unpack_from('>H', font, table + 16 + 2 * segments + 2 * segment)[0]
                if first != 0xffff:
                    points.update(range(first, last + 1))
    return points


def codepoints(charset, path):
    """The characters a subset keeps of the font at path."""
    if charset == 'latin':
        return [point for first, last in LATIN for point in range(first, last + 1)]
    if charset == 'no-flags':
        return sorted(point for point in characters(path)
                      if not (0x1f1e6 <= point <= 0x1f1ff or 0xe0020 <= point <= 0xe007f))
    raise RuntimeError('Unknown font subset: ' + charset)


def subsetter():
    """The host subsetter, built again when its source or HarfBuzz's changes."""
    from build_native_render import RECORDS, prepare
    record = next(x for x in RECORDS if x['name'] == 'harfbuzz')
    harfbuzz = prepare(record) / 'src'
    source = ROOT / 'tools/font_subset.cc'
    signature = hashlib.sha256((record['sha256'] + digest(source)).encode()).hexdigest()
    stamp = SUBSETTER.with_suffix('.sos2')
    if SUBSETTER.is_file() and stamp.is_file() and stamp.read_text().strip() == signature:
        return SUBSETTER
    SUBSETTER.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([os.environ.get('HOST_CXX', 'c++'), '-std=c++17', '-O2', '-DNDEBUG', '-DHB_NO_MT',
                    '-I', str(harfbuzz), str(harfbuzz / 'harfbuzz-subset.cc'), str(source),
                    '-o', str(SUBSETTER)], check=True)
    stamp.write_text(signature + '\n')
    return SUBSETTER


def subset(path, charset):
    """The font cut down to a character set, made once per font, set and tool."""
    points = ''.join(f'{point:x}\n' for point in codepoints(charset, path))
    tool = subsetter()
    signature = hashlib.sha256((digest(path) + points + digest(tool)).encode()).hexdigest()
    folder = CACHE / ('subset-' + charset)
    output, stamp = folder / path.name, folder / (path.name + '.sos2')
    if output.is_file() and stamp.is_file() and stamp.read_text().strip() == signature:
        return output
    folder.mkdir(parents=True, exist_ok=True)
    listing = folder / (path.name + '.codepoints')
    listing.write_text(points)
    subprocess.run([str(tool), str(path), str(listing), str(output)], check=True)
    stamp.write_text(signature + '\n')
    return output


def resources():
    result = {}
    for record in records():
        path = CACHE / record['file']
        if not valid(path, record):
            raise RuntimeError('Missing or changed font asset: ' + record['file'] + '; run make fetch')
        if 'subset' in record:
            path = subset(path, record['subset'])
        result['/fonts/' + record['file']] = path
    return result
