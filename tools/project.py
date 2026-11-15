#!/usr/bin/env python3
"""Small command interface for building, launching and packaging SOS2."""
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

HELP = """SOS2 — native ARM32 web desktop
  make                 Build the QEMU kernel and storage tools
  make fetch           Download pinned engine dependencies and Adreno firmware
  make engine          Build native WebKit and its resource bundle (JOBS=8)
  make run             Launch the browser in QEMU
  make phone           Package Redmi 2 boot.img, the installed system and the
                       app packages; never accesses the phone
  make publish         Push the installed system, update and apps to GitHub Pages
  make clean           Delete build/ entirely, including downloads, caches and disks
  make help            Show this list

run/phone: STORAGE=ram (default) or persistent; ENGINE=path/to/native-web.bundle
run:      ENGINE=none opens an empty desktop
          Four CPUs by default; RUN_ARGS='--cpus 1' selects a single CPU
          RUN_ARGS='--graphics virtio-gpu' selects experimental GPU display
          RUN_ARGS='--graphics virgl' adds GPU composition and retained textures
phone:    REDMI2_WIFI_FIRMWARE=folder (default third_party/redmi2-firmware)
          Optional REDMI2_WIFI_PROFILE=file
          REDMI2_ADRENO_FIRMWARE=folder enables GPU scrolling and scaling
          Optional REDMI2_RTC_ANCHOR=UNIX_SECONDS:RTC_COUNTER
See README.md and docs/redmi2.md for prerequisites and device limitations.
"""


def run(*args):
    subprocess.run([str(x) for x in args], cwd=ROOT, check=True)


def clean(root=ROOT):
    # Everything under build/ is regenerated or re-fetched: tracked firmware
    # lives in third_party/, and `make fetch` restores downloads.
    build = root / 'build'
    if build.is_symlink() or build.is_file():
        build.unlink()
    elif build.is_dir():
        shutil.rmtree(build)
    print('Removed build/; run make fetch and make engine to rebuild the engine.')


def launch(engine, storage, extra):
    args = [sys.executable, ROOT / 'tools/run_qemu.py', '--network']
    if engine:
        args += ['--bundle', engine]
    if storage == 'ram':
        args += ['--ram-only']
    else:
        disk = ROOT / 'build/native-arm32/data.img'
        if not disk.exists():
            disk.parent.mkdir(parents=True, exist_ok=True)
            run(ROOT / 'build/host/mkfs', disk, '64')
        args += ['--disk', disk]
    run(*args, *extra)


def release_name():
    """The build's name for the updater: UTC time and commit, with + when
    the checkout has uncommitted changes."""
    stamp = datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M')
    try:
        def git(*args):
            # No optional locks: status must not rewrite the index.
            return subprocess.run(['git', '--no-optional-locks', *args], cwd=ROOT, check=True,
                                  capture_output=True, text=True).stdout.strip()
        commit = git('rev-parse', '--short=7', 'HEAD')
        changed = git('status', '--porcelain', '--untracked-files=no')
    except (OSError, subprocess.CalledProcessError):
        return stamp
    return f"{stamp} {commit}{'+' if changed else ''}"


def phone(engine, storage):
    output = ROOT / ('build/redmi2-browser-ram/boot.img' if storage == 'ram'
                     else 'build/redmi2-browser/boot.img')
    args = [sys.executable, ROOT / 'tools/package_redmi2.py',
            '--kernel', ROOT / 'build/redmi2-protected-arm32/kernel.img',
            '--elf', ROOT / 'build/redmi2-protected-arm32/kernel.elf',
            '--dtb', ROOT / 'build/redmi2/boot.dtb', '--display-format', 'native',
            '--resources', engine, '--output', output, '--release', release_name()]
    if storage == 'ram':
        # The installed system shares the RAM image's compressed resources.
        args += ['--ram-only', '--install-dir', ROOT / 'build/redmi2-install',
                 '--apps-dir', ROOT / 'build/apps']
    for variable, option in (('REDMI2_WIFI_FIRMWARE', '--wifi-firmware-dir'),
                             ('REDMI2_ADRENO_FIRMWARE', '--adreno-firmware-dir'),
                             ('REDMI2_WIFI_PROFILE', '--wifi-profile'),
                             ('REDMI2_RTC_ANCHOR', '--rtc-anchor')):
        if os.environ.get(variable):
            args += [option, os.environ[variable]]
    run(*args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('help', 'clean', 'run', 'phone', 'publish'))
    parser.add_argument('--engine', default='build/native-webkit/native-web.bundle')
    parser.add_argument('--storage', choices=('ram', 'persistent'), default='ram')
    argv = sys.argv[1:]
    split = argv.index('--') if '--' in argv else len(argv)
    args = parser.parse_args(argv[:split])
    extra = argv[split + 1:]
    if extra and args.command != 'run':
        parser.error('additional launcher options are only supported by run')
    if args.command == 'help':
        print(HELP, end='')
        return
    if args.command == 'clean':
        clean()
        return
    if args.command == 'publish':
        try:
            run(sys.executable, ROOT / 'tools/publish_pages.py', ROOT / 'build/redmi2-install',
                ROOT / 'build/apps')
        except (OSError, subprocess.CalledProcessError) as error:
            parser.exit(1, str(error) + '\n')
        return
    engine = None if args.engine == 'none' else ROOT / args.engine
    if args.command == 'phone' and engine is None:
        parser.error('phone requires an engine bundle; run make engine first')
    if engine is not None and not engine.is_file():
        parser.error('engine bundle is missing; run make engine or set ENGINE to an existing bundle')
    try:
        if args.command == 'run':
            launch(engine, args.storage, extra)
        else:
            phone(engine, args.storage)
    except (OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
