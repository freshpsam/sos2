#!/usr/bin/env python3
"""Rebuild portable C/C++ libraries for the native SOS2 ARM32 ABI.

Only fetch accesses the network. Linux target libraries are never searched.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from native_sources import copy_if_changed, digest, download, run_logged, source_tree

ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'build/native-toolchain'
INSTALL=BASE/'install'
SYSROOT=INSTALL/'arm-none-eabi'
# ARMv7 code that QEMU's Cortex-A15 runs, scheduled for the phone's in-order
# Cortex-A53. sdk/sos2-arm32.cmake repeats this for CMake builds.
CPU=['-marm','-mcpu=cortex-a15','-mtune=cortex-a53','-mfpu=neon-vfpv4','-mfloat-abi=hard']
FLAGS=' '.join(['-O2','-g',*CPU,'-D__DYNAMIC_REENT__','-ffunction-sections','-fdata-sections',
                '-include',str(ROOT/'sdk/include/sos2/abi.h')])
LOCK=ROOT/'tools/browser/native-runtime.lock.json'


def records(): return json.loads(LOCK.read_text())['sources']


@contextmanager
def preserve_headers():
    # Upstream installers may overwrite headers (including generic/CPU-specific
    # overlays) even when the final contents do not change. Keep those mtimes
    # stable without suppressing dependency tracking for actual header changes.
    before={path:(digest(path),path.stat()) for path in (SYSROOT/'include').rglob('*') if path.is_file()}
    try:
        yield
    finally:
        for path,(checksum,info) in before.items():
            if path.is_file() and path.stat().st_mtime_ns!=info.st_mtime_ns and digest(path)==checksum:
                os.utime(path,ns=(info.st_atime_ns,info.st_mtime_ns))


def sources(fetch=False):
    """Download the pinned archives, or extract each one's tree once."""
    for record in records():
        path=BASE/'downloads'/record['url'].rsplit('/',1)[1]
        if fetch: download(record['url'],path,record['sha256'])
        else: source_tree(path,BASE/'src'/record.get('directory',path.name.removesuffix('.tar.gz').removesuffix('.tar.xz')),record['sha256'])


def run(command,directory,label,env=None):
    directory.mkdir(parents=True,exist_ok=True)
    run_logged(command,BASE/(label+'.log'),'Native runtime',directory,env)


def newlib(jobs):
    sources()
    build=BASE/'newlib-build'; source=BASE/'src/newlib-4.6.0.20260123'
    options=[str(source/'configure'),'--target=arm-none-eabi','--prefix='+str(INSTALL),
        '--disable-multilib','--disable-nls','--disable-newlib-supplied-syscalls',
        '--enable-newlib-multithread','--enable-newlib-retargetable-locking',
        '--enable-newlib-io-long-long',
        '--disable-libgloss','CFLAGS_FOR_TARGET='+FLAGS]
    signature=hashlib.sha256(json.dumps([options,digest(ROOT/'sdk/include/sos2/abi.h')]).encode()).hexdigest()
    stamp=build/'.sos2-config'
    if not stamp.exists() or stamp.read_text().strip()!=signature:
        if stamp.exists():shutil.rmtree(build)
        run(options,build,'newlib-configure'); stamp.write_text(signature+'\n')
    run(['make','-j'+str(jobs),'all-target-newlib'],build,'newlib-build')
    with preserve_headers():run(['make','install-target-newlib'],build,'newlib-install')
    print('Native Newlib:',SYSROOT,flush=True)


def sdk_includes(cxx=False):
    """The sysroot, SDK and kernel ABI headers; with cxx, libstdc++'s first."""
    library=['-isystem',str(SYSROOT/'include/c++/14.2.0'),'-isystem',str(SYSROOT/'include/c++/14.2.0/arm-none-eabi')] if cxx else []
    return ['--sysroot='+str(SYSROOT),*library,'-isystem',str(SYSROOT/'include'),
        '-I'+str(ROOT/'sdk/include'),'-I'+str(ROOT/'include'),'-include',str(ROOT/'sdk/include/sos2/features.h')]


def sdk_flags():
    """Compiler flags for SDK code (the runtime libraries, libsos2 and apps)."""
    return FLAGS.split()+sdk_includes()


def link_flags(*options):
    """Link a static SOS2 program: the SDK's start code, memory layout and all of
    libsos2. options are extra linker options; link_libraries() goes last."""
    return ['-static','-nostartfiles','-T',str(ROOT/'sdk/runtime/linker.ld'),
        '-Wl,'+','.join(['-z','max-page-size=4096','-z','noexecstack','--gc-sections',*options]),
        '-L'+str(SYSROOT/'lib'),'-Wl,--whole-archive',str(BASE/'sdk/libsos2.a'),'-Wl,--no-whole-archive']


def link_libraries(cxx=False):
    """Newlib and libgcc (with cxx, libstdc++ too), resolved as one group."""
    return ['-Wl,--start-group',*(['-lstdc++'] if cxx else []),'-lc','-lm','-lgcc','-Wl,--end-group']


def sdk():
    if not (SYSROOT/'lib/libc.a').exists(): raise RuntimeError('Build native Newlib first')
    build=BASE/'sdk';build.mkdir(parents=True,exist_ok=True)
    objects=[]
    for filename in ('start.S','start.c','system.c','threads.c','semaphore.c','inet.c','network.c'):
        output=build/(filename+'.o');objects.append(str(output))
        run(['arm-none-eabi-gcc',*sdk_flags(),'-Wall','-Wextra','-Werror=implicit-function-declaration',
             '-c',str(ROOT/'sdk/runtime'/filename),'-o',str(output)],build,'sdk-'+filename)
    run(['arm-none-eabi-ar','rcs',str(build/'libsos2.a'),*objects],build,'sdk-archive')


def libstdcxx(jobs):
    sources()
    source=BASE/'src/gcc-14.2.0';build=BASE/'libstdcxx-build'
    # The installed bare-metal compiler reports "single". Select the upstream
    # POSIX gthread implementation explicitly for our native pthread library.
    configure=source/'libstdc++-v3/configure'
    original="target_thread_file=`$CXX -v 2>&1 | sed -n 's/^Thread model: //p'`"
    replacement='target_thread_file=posix # SOS2 native pthreads'
    content=configure.read_text()
    if content.count(original)==2: configure.write_text(content.replace(original,replacement))
    elif content.count(replacement)!=2: raise RuntimeError('Unexpected libstdc++ thread selection')
    (BASE/'libgcc').mkdir(exist_ok=True)
    copy_if_changed(source/'libgcc/gthr-posix.h',BASE/'libgcc/gthr-default.h')
    import shlex
    flags=shlex.join(sdk_flags()+['-DGTHREAD_USE_WEAK=0','-D_GLIBCXX_GTHREAD_USE_WEAK=0'])
    env=os.environ.copy()
    env.update(CC='arm-none-eabi-gcc',CXX='arm-none-eabi-g++',AR='arm-none-eabi-ar',RANLIB='arm-none-eabi-ranlib',
               CFLAGS=flags,CXXFLAGS=flags,CPPFLAGS=flags,
               LDFLAGS='-nodefaultlibs -nostartfiles -Wl,-T,'+str(ROOT/'sdk/runtime/linker.ld')+' -L'+str(SYSROOT/'lib')+' -L'+str(BASE/'sdk'),
               LIBS='-Wl,--whole-archive -lsos2 -Wl,--no-whole-archive -Wl,--start-group -lc -lm -lgcc -Wl,--end-group')
    options=[str(configure),'--build=x86_64-pc-linux-gnu','--host=arm-none-eabi','--target=arm-none-eabi',
        '--prefix='+str(INSTALL),'--libdir='+str(SYSROOT/'lib'),'--with-gxx-include-dir='+str(SYSROOT/'include/c++/14.2.0'),
        '--disable-shared','--disable-multilib','--disable-nls','--disable-libstdcxx-pch','--disable-libstdcxx-verbose',
        '--enable-libstdcxx-threads','--enable-libstdcxx-time=yes','--with-newlib']
    signature=hashlib.sha256(json.dumps([options,flags,digest(ROOT/'sdk/include/sos2/features.h'),'clean-config-v2']).encode()).hexdigest();stamp=build/'.sos2-config'
    if not stamp.exists() or stamp.read_text().strip()!=signature:
        if stamp.exists():shutil.rmtree(build)
        run(options,build,'libstdcxx-configure',env);stamp.write_text(signature+'\n')
    run(['make','-j'+str(jobs)],build,'libstdcxx-build',env)
    with preserve_headers():run(['make','install'],build,'libstdcxx-install',env)
    print('Native C++ library:',SYSROOT/'lib/libstdc++.a',flush=True)


def icu(jobs):
    sources();source=BASE/'src/icu/source';host=BASE/'icu-host';build=BASE/'icu-build'
    copy_if_changed(ROOT/'tools/browser/icu-sos2.mk',source/'config/mh-sos2')
    common=['--disable-shared','--enable-static','--disable-tests','--disable-samples','--disable-extras','--disable-dyload']
    if not (host/'config.status').exists():
        run([str(source/'configure'),'--prefix='+str(BASE/'icu-host-install'),*common],host,'icu-host-configure')
    run(['make','-j'+str(jobs)],host,'icu-host-build')
    import shlex
    includes=SYSROOT/'include/c++/14.2.0'
    flags=sdk_flags()+['-DU_HAVE_MMAP=0']
    env=os.environ.copy()
    env.update(CC='arm-none-eabi-gcc',CXX='arm-none-eabi-g++',AR='arm-none-eabi-ar',RANLIB='arm-none-eabi-ranlib',icu_cv_host_frag='mh-sos2',
        CFLAGS=shlex.join(flags),CXXFLAGS=shlex.join(flags+['-I'+str(includes),'-I'+str(includes/'arm-none-eabi')]),
        LDFLAGS='-nostartfiles -Wl,-T,'+str(ROOT/'sdk/runtime/linker.ld')+' -L'+str(SYSROOT/'lib')+' -L'+str(BASE/'sdk'),
        LIBS='-Wl,--whole-archive -lsos2 -Wl,--no-whole-archive -Wl,--start-group -lstdc++ -lc -lm -lgcc -Wl,--end-group')
    options=[str(source/'configure'),'--build=x86_64-pc-linux-gnu','--host=arm-none-eabi',
        '--prefix='+str(SYSROOT),'--with-cross-build='+str(host),'--with-data-packaging=static','--disable-tools',*common]
    signature=hashlib.sha256(json.dumps([options,flags,'mh-sos2',digest(ROOT/'sdk/include/sos2/abi.h')]).encode()).hexdigest();stamp=build/'.sos2-config'
    if not stamp.exists() or stamp.read_text().strip()!=signature:
        if stamp.exists():shutil.rmtree(build)
        run(options,build,'icu-configure',env);stamp.write_text(signature+'\n')
    run(['make','-j'+str(jobs)],build,'icu-build',env)
    with preserve_headers():run(['make','install'],build,'icu-install',env)
    print('Native ICU:',SYSROOT/'lib/libicuuc.a',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=('fetch','newlib','sdk','libstdcxx','icu'))
    parser.add_argument('-j','--jobs',type=int,default=8)
    args=parser.parse_args()
    if args.jobs<1: parser.error('jobs must be positive')
    try:
        if args.command=='fetch': sources(fetch=True)
        elif args.command=='newlib': newlib(args.jobs)
        elif args.command=='sdk': sdk()
        elif args.command=='libstdcxx': libstdcxx(args.jobs)
        elif args.command=='icu': icu(args.jobs)
    except (OSError,RuntimeError,subprocess.CalledProcessError) as error: parser.exit(1,str(error)+'\n')


if __name__=='__main__': main()
