#!/usr/bin/env python3
"""Fetch pinned browser fonts explicitly; package only verified local assets."""
import argparse
import hashlib
import json
from pathlib import Path
from native_sources import download

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / 'build/native-webkit/fonts'
MANIFEST = ROOT / 'tools/browser/fonts.lock.json'


def records():
    return json.loads(MANIFEST.read_text())['assets']


def valid(path, record):
    if not path.is_file() or path.stat().st_size != record['bytes']:
        return False
    return hashlib.sha256(path.read_bytes()).hexdigest() == record['sha256']


def fetch():
    for record in records():
        download(record['url'], CACHE / record['file'], record['sha256'])


def resources():
    result = {}
    for record in records():
        path = CACHE / record['file']
        if not valid(path, record):
            raise RuntimeError('Missing or changed font asset: ' + record['file'] +
                               '; run python3 tools/browser_fonts.py fetch')
        result['/fonts/' + record['file']] = path
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('fetch', 'verify'))
    args = parser.parse_args()
    try:
        if args.command == 'fetch':
            fetch()
        print('Verified browser font assets:', ', '.join(resources()))
    except (OSError, RuntimeError) as error:
        parser.exit(1, str(error) + '\n')
