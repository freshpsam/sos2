#!/usr/bin/env python3
"""Small command interface for building, launching and packaging SOS2."""
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import shutil
import subprocess
import sys

import arch

ROOT = Path(__file__).resolve().parents[1]

HELP = """SOS2 — native ARM web desktop
  make                 Build the QEMU kernel and storage tools
  make fetch           Download pinned engine dependencies and Adreno firmware
  make engine          Build native WebKit and its resource bundle (JOBS=8)
  make run             Launch the browser in QEMU
  make phone           Package Redmi 2 boot.img, the installed system and the
                       app packages; never accesses the phone
  make publish         Push each built device's installed system, update and
                       apps to GitHub Pages (PUBLISH=redmi2 or gta7lite: one)
  make tablet          Package the Galaxy Tab A7 Lite's boot image and installed
                       system (arm64); never accesses the tablet
  make clean           Delete build/ entirely, including downloads, caches and disks
  make help            Show this list

ARCH=arm32 (default) or arm64: build and run QEMU's ARM or AArch64 system;
          phone sets ARM32 (the Redmi 2), tablet arm64
run/phone: STORAGE=ram (default) or persistent; ENGINE=path/to/native-web.bundle
run:      ENGINE=none opens an empty desktop
          Four CPUs by default; RUN_ARGS='--cpus 1' selects a single CPU
          RUN_ARGS='--graphics virtio-gpu' selects experimental GPU display
          RUN_ARGS='--graphics virgl' adds GPU composition and retained textures
          RUN_ARGS='--jsc useDFGJIT=false' sets JavaScriptCore options
          (useJIT=false: interpreter only; useDFGJIT=false: up to the baseline JIT)
          Sound through PulseAudio or PipeWire when one runs; RUN_ARGS='--audio
          none' leaves it out, '--audio wav:FILE' records it
phone:    REDMI2_WIFI_FIRMWARE=folder (default third_party/redmi2-firmware)
          Optional REDMI2_WIFI_PROFILE=file
          REDMI2_ADRENO_FIRMWARE=folder (default build/redmi2-adreno once
          fetched) gives GPU scrolling and scaling
          Optional REDMI2_JSC=useDFGJIT=false: JavaScriptCore options for pages
tablet:   GTA7LITE_STOCK=folder with the stock firmware's boot.img and vbmeta.img
          adds boot.img and odin4's boot.tar and vbmeta-disabled.tar, and the
          installed system's flash-boot.img
          GTA7LITE_TOUCH_FIRMWARE=folder of novatek_ts_*_fw.bin
          (default GTA7LITE_STOCK/touch-firmware) gives the tablet touch
          GTA7LITE_WIFI_FIRMWARE=folder of the vendor's soc1_0 firmware
          (default GTA7LITE_STOCK/wifi-firmware) gives the tablet Wi-Fi
          Optional GTA7LITE_WIFI_PROFILE=file, joined at boot
          Optional GTA7LITE_JSC=options: JavaScriptCore options for pages
See README.md, docs/redmi2.md and docs/gta7lite.md for prerequisites and device
limitations.
"""


def run(*args):
    subprocess.run([str(x) for x in args], cwd=ROOT, check=True)


def clean():
    # Everything under build/ is regenerated or re-fetched: tracked firmware
    # lives in third_party/, and `make fetch` restores downloads.
    build = ROOT / 'build'
    if build.is_symlink() or build.is_file():
        build.unlink()
    elif build.is_dir():
        shutil.rmtree(build)
    print('Removed build/; run make fetch and make engine to rebuild the engine.')


def launch(engine, storage, extra):
    args = [sys.executable, ROOT / 'tools/run_qemu.py', '--arch', arch.ARCH]
    if engine:
        args += ['--bundle', engine]
    if storage == 'ram':
        args += ['--ram-only']
    else:
        disk = ROOT / f'build/native-{arch.ARCH}/data.img'
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
            '--dtb', ROOT / 'build/redmi2/boot.dtb',
            '--resources', engine, '--output', output, '--release', release_name()]
    if storage == 'ram':
        # The installed system shares the RAM image's compressed resources.
        args += ['--ram-only', '--install-dir', ROOT / 'build/redmi2-install',
                 '--apps-dir', ROOT / 'build/apps']
    for variable, option in (('REDMI2_WIFI_FIRMWARE', '--wifi-firmware-dir'),
                             ('REDMI2_ADRENO_FIRMWARE', '--adreno-firmware-dir'),
                             ('REDMI2_WIFI_PROFILE', '--wifi-profile'),
                             ('REDMI2_JSC', '--jsc')):
        if os.environ.get(variable):
            args += [option, os.environ[variable]]
    run(*args)


def tablet(engine):
    args = [sys.executable, ROOT / 'tools/package_gta7lite.py',
            '--kernel', ROOT / 'build/gta7lite-arm64/kernel.img', '--resources', engine,
            '--release', release_name(), '--output-dir', ROOT / 'build/gta7lite',
            '--apps-dir', ROOT / 'build/apps-arm64']
    if os.environ.get('GTA7LITE_STOCK'):
        # The installed system for make publish, beside the flash images.
        args += ['--stock', os.environ['GTA7LITE_STOCK'], '--install-dir', ROOT / 'build/gta7lite-install']
    touch = os.environ.get('GTA7LITE_TOUCH_FIRMWARE')
    if touch and Path(touch).is_dir():
        args += ['--touch-firmware', touch]
    elif touch:
        print(f'GTA7LITE_TOUCH_FIRMWARE: no folder {touch}, so no touch')
    wifi = os.environ.get('GTA7LITE_WIFI_FIRMWARE')
    if wifi and Path(wifi).is_dir():
        args += ['--wifi-firmware', wifi]
    elif wifi:
        print(f'GTA7LITE_WIFI_FIRMWARE: no folder {wifi}, so no Wi-Fi')
    if os.environ.get('GTA7LITE_WIFI_PROFILE'):
        args += ['--wifi-profile', os.environ['GTA7LITE_WIFI_PROFILE']]
    if os.environ.get('GTA7LITE_JSC'):
        args += ['--jsc', os.environ['GTA7LITE_JSC']]
    run(*args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('help', 'clean', 'run', 'phone', 'publish', 'tablet'))
    parser.add_argument('--engine', default=f'build/native-webkit{arch.SUFFIX}/native-web.bundle')
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
    if arch.ARM64 and args.command == 'phone':
        parser.error('the Redmi 2 runs ARM32: phone needs ARCH=arm32')
    if not arch.ARM64 and args.command == 'tablet':
        parser.error('the Galaxy Tab A7 Lite runs AArch64: tablet needs ARCH=arm64')
    if args.command == 'publish':
        try:
            devices = os.environ.get('PUBLISH', '').split()
            run(sys.executable, ROOT / 'tools/publish_pages.py', *(f'--device={d}' for d in devices))
        except (OSError, subprocess.CalledProcessError) as error:
            parser.exit(1, str(error) + '\n')
        return
    engine = None if args.engine == 'none' else ROOT / args.engine
    if args.command in ('phone', 'tablet') and engine is None:
        parser.error(f'{args.command} requires an engine bundle; run make engine first')
    if engine is not None and not engine.is_file():
        parser.error('engine bundle is missing; run make engine or set ENGINE to an existing bundle')
    try:
        if args.command == 'run':
            launch(engine, args.storage, extra)
        elif args.command == 'tablet':
            tablet(engine)
        else:
            phone(engine, args.storage)
    except (OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
