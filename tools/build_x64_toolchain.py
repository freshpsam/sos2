#!/usr/bin/env python3
"""The x86_64-elf cross toolchain: binutils and GCC 14.2 (C and C++, with
libgcc), built from pinned GNU sources into .tools/x86_64-elf, where
tools/arch.py finds it. There is no prebuilt one like the Arm GNU
Toolchain that serves ARM32 and arm64.

GCC is the runtime's pinned 14.2.0 (tools/browser/native-runtime.lock.json),
with GMP, MPFR and MPC built in its tree at the versions its
contrib/download_prerequisites names. Newlib and libstdc++ stay
tools/build_native_runtime.py's. Unlike aarch64-none-elf, this GCC has
native TLS (%fs), and its libgcc is position-independent (-fpie): user code
is linked at 0x1010000000 (sdk/runtime/linker-x64.ld), beyond the small
code model's absolute 32-bit addresses. Its static constructors go in
.init_array, which Newlib's __libc_init_array runs, as with the Arm
toolchains: configure guesses .ctors for any cross compiler. Only fetch
accesses the network.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
from native_sources import download, run_logged, source_tree

ROOT = Path(__file__).resolve().parents[1]
TRIPLE = 'x86_64-elf'
BASE = ROOT/'build/x64-toolchain'
PREFIX = ROOT/'.tools'/TRIPLE
# The runtime's downloads, where tools/build_native_runtime.py keeps GCC.
DOWNLOADS = ROOT/'build/native-toolchain/downloads'
LOCK = ROOT/'tools/browser/x64-toolchain.lock.json'
RUNTIME_LOCK = ROOT/'tools/browser/native-runtime.lock.json'
# The SDK's CPU level (tools/arch.py) for libgcc, which the kernel links too;
# kernel code never takes an interrupt on its own stack, so libgcc's red zone
# is harmless there.
TARGET_FLAGS = '-O2 -g -fpie -march=x86-64-v2 -mtune=generic'
BINUTILS = ['--disable-nls', '--disable-werror', '--disable-gdb', '--disable-gdbserver', '--disable-sim',
            '--disable-gprofng', '--enable-deterministic-archives']
GCC = ['--disable-nls', '--enable-languages=c,c++', '--without-headers', '--with-newlib', '--disable-multilib',
       '--disable-shared', '--disable-threads', '--enable-tls', '--enable-initfini-array', '--without-isl',
       '--disable-libssp', '--disable-libquadmath', '--disable-libgomp', '--disable-libatomic',
       '--disable-libsanitizer', '--disable-libvtv', '--with-gnu-as', '--with-gnu-ld',
       '--with-pkgversion=SOS2 x86_64-elf']


def records():
    gcc = [r for r in json.loads(RUNTIME_LOCK.read_text())['sources'] if r['name'] == 'gcc']
    return {r['name']: r for r in json.loads(LOCK.read_text())['sources'] + gcc}


def archive(record): return DOWNLOADS/record['url'].rsplit('/', 1)[1]


def tree(record):
    name = record.get('directory', archive(record).name.removesuffix('.tar.xz'))
    return source_tree(archive(record), BASE/'src'/name, record['sha256'])


def fetch():
    for record in records().values():
        download(record['url'], archive(record), record['sha256'])


def run(command, directory, label, env):
    directory.mkdir(parents=True, exist_ok=True)
    run_logged(command, BASE/(label+'.log'), 'x64 toolchain', directory, env)


def build(jobs):
    sources = records()
    signature = hashlib.sha256(json.dumps([sorted((k, v['sha256']) for k, v in sources.items()), BINUTILS, GCC,
                                           TARGET_FLAGS, str(PREFIX)]).encode()).hexdigest()
    stamp = PREFIX/'.sos2-toolchain'
    if stamp.is_file() and stamp.read_text().strip() == signature and (PREFIX/'bin'/(TRIPLE+'-g++')).is_file():
        print('x64 toolchain is current:', PREFIX, flush=True)
        return
    stamp.unlink(missing_ok=True)
    binutils, gcc = tree(sources['binutils']), tree(sources['gcc'])
    # GMP, MPFR and MPC build in GCC's tree, as contrib/download_prerequisites
    # links them.
    for name in ('gmp', 'mpfr', 'mpc'):
        link = gcc/name
        if not link.is_symlink(): link.symlink_to(os.path.relpath(tree(sources[name]), gcc))
    env = os.environ.copy()
    env['PATH'] = os.pathsep.join([str(PREFIX/'bin'), env.get('PATH', '')])
    build_dir = BASE/'binutils-build'
    run([binutils/'configure', '--target='+TRIPLE, '--prefix='+str(PREFIX), *BINUTILS], build_dir,
        'binutils-configure', env)
    # No Info manuals: makeinfo is not a build requirement.
    run(['make', '-j', str(jobs), 'MAKEINFO=true'], build_dir, 'binutils-build', env)
    run(['make', 'MAKEINFO=true', 'install'], build_dir, 'binutils-install', env)
    build_dir = BASE/'gcc-build'
    run([gcc/'configure', '--target='+TRIPLE, '--prefix='+str(PREFIX), *GCC], build_dir, 'gcc-configure', env)
    flags = ['CFLAGS_FOR_TARGET='+TARGET_FLAGS, 'CXXFLAGS_FOR_TARGET='+TARGET_FLAGS, 'MAKEINFO=true']
    run(['make', '-j', str(jobs), *flags, 'all-gcc', 'all-target-libgcc'], build_dir, 'gcc-build', env)
    run(['make', *flags, 'install-gcc', 'install-target-libgcc'], build_dir, 'gcc-install', env)
    stamp.write_text(signature+'\n')
    print('x64 toolchain:', PREFIX, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('fetch', 'build'))
    parser.add_argument('-j', '--jobs', type=int, default=8)
    args = parser.parse_args()
    try:
        fetch() if args.command == 'fetch' else build(args.jobs)
    except RuntimeError as error:
        parser.exit(1, f'{error}\n(the toolchain\'s sources come with make ARCH=x64 fetch)\n')


if __name__ == '__main__':
    main()
