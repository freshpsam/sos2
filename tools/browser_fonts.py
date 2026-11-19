#!/usr/bin/env python3
"""Fetch pinned browser fonts explicitly; package only verified local assets.

An asset with a "subset" is packaged cut down to that character set, by
tools/font_subset.cc built from the pinned HarfBuzz source the browser links:
"gb2312" keeps Simplified Chinese, SOS2's language beside English, small
enough for the whole system to fit a 32 MiB boot partition."""
import hashlib
import json
import os
from pathlib import Path
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


def codepoints(charset):
    """GB2312, the character set of Simplified Chinese text, as Python's own
    codec decodes it, with ASCII, CJK punctuation, kana and the halfwidth and
    fullwidth forms."""
    if charset != 'gb2312':
        raise RuntimeError('Unknown font subset: ' + charset)
    points = set(range(0x20, 0x7f)) | set(range(0x3000, 0x3100)) | set(range(0xff00, 0xfff0))
    for lead in range(0xa1, 0xf8):
        for trail in range(0xa1, 0xff):
            try:
                text = bytes((lead, trail)).decode('gb2312')
            except UnicodeDecodeError:
                continue
            points.add(ord(text))
    return sorted(points)


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
    points = ''.join(f'{point:x}\n' for point in codepoints(charset))
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
