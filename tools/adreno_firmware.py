#!/usr/bin/env python3
"""Fetch explicitly or validate the pinned, unmodified A306 firmware pair."""
import argparse
import hashlib
from pathlib import Path
from urllib.request import urlopen

REVISION = '69e51bb26f3fa3e118dd770f1962a93bed690dc1'
BASE = 'https://gitlab.com/kernel-firmware/linux-firmware/-/raw/'+REVISION+'/'
FILES = {
    'a300_pm4.fw': ('qcom/a300_pm4.fw', 9220, 'f78c57e879e96a2c0ceeea31dbd30f7f1d4526f9c876e80481c654f08daaecd3'),
    'a300_pfp.fw': ('qcom/a300_pfp.fw', 1156, 'b6e66400d46d52dc343d370b94362d564a765994bda47dc026961dd47a9ca7a9'),
    'LICENSE.qcom.txt': ('LICENSES/LICENSE.qcom', 13962, 'be904cd28cb292b80cdb6cf412ab0d9159d431671e987ad433c1f62e0988a9bc'),
    'NOTICE.txt': ('LICENSES/NOTICE.qcom', 23966, 'fa43e1b9a13b341a07adca9dbe73d0f9072d7966fdfe811c01f0dd2872d7309a'),
}


def validate(name, data):
    _, size, digest = FILES[name]
    if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
        raise ValueError('Adreno '+name+' does not match the pinned firmware/notice')
    return data


def resources(directory):
    return {('/firmware/adreno/'+name).encode(): validate(name, (directory/name).read_bytes())
            for name in FILES}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('fetch', 'check'))
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    if args.action == 'fetch':
        # Fetch/validate all bytes before publishing. Never replace an existing
        # mismatching backup. Packaging itself never downloads anything.
        data = {}
        for name, (path, _, _) in FILES.items():
            target = args.directory/name
            content = target.read_bytes() if target.exists() else urlopen(BASE+path, timeout=30).read()
            data[name] = validate(name, content)
        args.directory.mkdir(parents=True, exist_ok=True)
        for name, content in data.items():
            target = args.directory/name
            if not target.exists():
                with target.open('xb') as f:
                    f.write(content)
    resources(args.directory)
    print('Verified pinned A306 firmware and redistribution notices')


if __name__ == '__main__':
    main()
