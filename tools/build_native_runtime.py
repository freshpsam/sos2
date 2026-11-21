#!/usr/bin/env python3
"""Rebuild portable C/C++ libraries for the native SOS2 ABI of the target
architecture (tools/arch.py).

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
import arch
from native_sources import copy_if_changed, digest, download, run_logged, source_tree

ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/('build/native-toolchain'+arch.SUFFIX)
# Pinned archives are the same for every architecture: make fetch fills ARM32's.
DOWNLOADS=ROOT/'build/native-toolchain/downloads'
INSTALL=BASE/'install'
SYSROOT=INSTALL/arch.TRIPLE
CPU=arch.CPU
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
        path=DOWNLOADS/record['url'].rsplit('/',1)[1]
        if fetch: download(record['url'],path,record['sha256'])
        else: source_tree(path,BASE/'src'/record.get('directory',path.name.removesuffix('.tar.gz').removesuffix('.tar.xz')),record['sha256'])


def run(command,directory,label,env=None):
    directory.mkdir(parents=True,exist_ok=True)
    run_logged(command,BASE/(label+'.log'),'Native runtime',directory,env)


def newlib(jobs):
    sources()
    build=BASE/'newlib-build'; source=BASE/'src/newlib-4.6.0.20260123'
    options=[str(source/'configure'),'--target='+arch.TRIPLE,'--prefix='+str(INSTALL),
        '--disable-multilib','--disable-nls','--disable-newlib-supplied-syscalls',
        '--enable-newlib-multithread','--enable-newlib-retargetable-locking',
        '--enable-newlib-io-long-long',
        '--disable-libgloss','CFLAGS_FOR_TARGET='+FLAGS]
    # Newlib's top-level configure does not consume ARM32_PREFIX/ARM64_PREFIX.
    # Pass the tools explicitly; keeping them in options also invalidates the
    # configuration stamp when the selected toolchain changes.
    options += [name+'_FOR_TARGET='+arch.tool(tool) for name,tool in (
        ('CC','gcc'),('GCC','gcc'),('CXX','g++'),('AR','ar'),('AS','as'),('LD','ld'),
        ('NM','nm'),('RANLIB','ranlib'),('READELF','readelf'),('OBJCOPY','objcopy'),
        ('OBJDUMP','objdump'),('STRIP','strip'))]
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
    library=['-isystem',str(SYSROOT/'include/c++/14.2.0'),'-isystem',str(SYSROOT/'include/c++/14.2.0'/arch.TRIPLE)] if cxx else []
    return ['--sysroot='+str(SYSROOT),*library,'-isystem',str(SYSROOT/'include'),
        '-I'+str(ROOT/'sdk/include'),'-I'+str(ROOT/'include'),'-include',str(ROOT/'sdk/include/sos2/features.h')]


def sdk_flags():
    """Compiler flags for SDK code (the runtime libraries, libsos2 and apps)."""
    return FLAGS.split()+sdk_includes()


def link_flags(*options):
    """Link a static SOS2 program: the SDK's start code, memory layout and all of
    libsos2. options are extra linker options; link_libraries() goes last."""
    return ['-static','-nostartfiles','-T',str(arch.LINKER_SCRIPT),
        '-Wl,'+','.join(['-z','max-page-size=4096','-z','noexecstack','--gc-sections',*arch.LINK,*options]),
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
        run([arch.tool('gcc'),*sdk_flags(),'-Wall','-Wextra','-Werror=implicit-function-declaration',
             '-c',str(ROOT/'sdk/runtime'/filename),'-o',str(output)],build,'sdk-'+filename)
    run([arch.tool('ar'),'rcs',str(build/'libsos2.a'),*objects],build,'sdk-archive')


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
    env.update(CC=arch.tool('gcc'),CXX=arch.tool('g++'),AR=arch.tool('ar'),RANLIB=arch.tool('ranlib'),
               CFLAGS=flags,CXXFLAGS=flags,CPPFLAGS=flags,
               LDFLAGS='-nodefaultlibs -nostartfiles -Wl,-T,'+str(arch.LINKER_SCRIPT)+' -L'+str(SYSROOT/'lib')+' -L'+str(BASE/'sdk')+
                       (' -Wl,'+','.join(arch.LINK) if arch.LINK else ''),
               LIBS='-Wl,--whole-archive -lsos2 -Wl,--no-whole-archive -Wl,--start-group -lc -lm -lgcc -Wl,--end-group')
    options=[str(configure),'--build=x86_64-pc-linux-gnu','--host='+arch.TRIPLE,'--target='+arch.TRIPLE,
        '--prefix='+str(INSTALL),'--libdir='+str(SYSROOT/'lib'),'--with-gxx-include-dir='+str(SYSROOT/'include/c++/14.2.0'),
        '--disable-shared','--disable-multilib','--disable-nls','--disable-libstdcxx-pch','--disable-libstdcxx-verbose',
        '--enable-libstdcxx-threads','--enable-libstdcxx-time=yes','--with-newlib']
    signature=hashlib.sha256(json.dumps([options,flags,arch.prefix(),env['LDFLAGS'],digest(ROOT/'sdk/include/sos2/features.h'),'clean-config-v2']).encode()).hexdigest();stamp=build/'.sos2-config'
    if not stamp.exists() or stamp.read_text().strip()!=signature:
        if stamp.exists():shutil.rmtree(build)
        run(options,build,'libstdcxx-configure',env);stamp.write_text(signature+'\n')
    run(['make','-j'+str(jobs)],build,'libstdcxx-build',env)
    with preserve_headers():run(['make','install'],build,'libstdcxx-install',env)
    print('Native C++ library:',SYSROOT/'lib/libstdc++.a',flush=True)


# The browser's ICU data for SOS2's language, English. Whole, it takes 33 MB,
# a quarter of the engine; trimmed, 4 MB. Kept: Unicode properties,
# normalization and IDNA, the break rules, time zones and the other
# supplemental data, the converters WebKit's TextCodecICU decodes with
# (WebCore/PAL/pal/text/TextCodecICU.cpp, by the names ICU 78 gives them;
# WebKit decodes the rest itself) but EUC-TW's, and in every locale tree, the
# break rules' included, the data of root and English, which its index then
# lists alone. Dropped: transliteration, spelled-out numbers, the word
# dictionaries (Chinese and Japanese, Southeast Asian), the line rules of
# Chinese and Japanese and the sentence rules of Greek, whose locales go, the
# other converters and locales; dates, numbers and collation in other
# languages fall back to the root locale's, and text in EUC-TW decodes to
# nothing. English alone keeps the Nexus 5's system within its 22 MiB boot
# partition (docs/nexus5.md).
ICU_LOCALES={'root','en','en_001','en_US','en_US_POSIX'}
ICU_DROPPED_ITEMS={'brkitr/burmesedict.dict','brkitr/cjdict.dict','brkitr/khmerdict.dict',
    'brkitr/laodict.dict','brkitr/thaidict.dict','brkitr/line_cj.brk','brkitr/line_loose_cj.brk',
    'brkitr/line_loose_phrase_cj.brk','brkitr/line_normal_cj.brk',
    'brkitr/line_normal_phrase_cj.brk','brkitr/line_phrase_cj.brk','brkitr/sent_el.brk'}
ICU_CONVERTERS={'ibm-912_P100-1995','ibm-914_P100-1995','ibm-915_P100-1995','iso-8859_10-1998',
    'ibm-921_P100-1995','iso-8859_14-1998','ibm-923_P100-1998','ibm-878_P100-1996','macos-0_2-10.2',
    'ibm-5346_P100-1998','ibm-5347_P100-1998','ibm-5350_P100-1998','ibm-9448_X100-2005',
    'ibm-5354_P100-1998','macos-7_3-10.2','macos-6_2-10.4','macos-29-10.2','macos-35-10.2'}
ICU_LOCALE_TREES=('','brkitr/','coll/','curr/','lang/','region/','unit/','zone/')
ICU_DROPPED_TREES=('rbnf/','translit/')


def trim_icu_data():
    """Replace the installed libicudata.a with the trimmed data, built once
    per input and copied with its own time, so the engine relinks only when
    the data changes."""
    import re
    built=sorted((BASE/'icu-build/data/out/tmp').glob('icudt*l.dat'))
    if len(built)!=1: raise RuntimeError('ICU data: expected one icudt*l.dat in the ICU build')
    data=built[0];entry=data.stem[:-1];host=BASE/'icu-host/bin';work=BASE/'icu-trim'
    library=work/'libicudata.a';stamp=work/'.sos2-trim'
    signature=hashlib.sha256(json.dumps([digest(data),sorted(ICU_LOCALES),sorted(ICU_CONVERTERS),
        sorted(ICU_DROPPED_ITEMS),ICU_LOCALE_TREES,ICU_DROPPED_TREES,CPU,arch.prefix()]).encode()).hexdigest()
    if not (library.exists() and stamp.exists() and stamp.read_text().strip()==signature):
        if work.exists():shutil.rmtree(work)
        (work/'bundles').mkdir(parents=True)
        def tool(name,*args,directory=work):
            return subprocess.run([str(host/name),*map(str,args)],cwd=directory,check=True,capture_output=True,text=True).stdout
        items=tool('icupkg','-l',data).split()
        missing=sorted(name for name in ICU_CONVERTERS if name+'.cnv' not in items)
        if missing:raise RuntimeError('ICU data has no converter '+', '.join(missing))
        indexes=[tree+'res_index.res' for tree in ICU_LOCALE_TREES]
        locale=re.compile(r'(?:[a-z]{2,3}(?:_[A-Za-z0-9]*)*|root)\.res')
        removed=[]
        for item in items:
            tree=item[:item.rfind('/')+1];name=item[len(tree):]
            if tree in ICU_DROPPED_TREES or item in ICU_DROPPED_ITEMS or (
                    item.endswith('.cnv') and item[:-4] not in ICU_CONVERTERS) or (
                    tree in ICU_LOCALE_TREES and item not in indexes and locale.fullmatch(name) and
                    name[:-4] not in ICU_LOCALES):
                removed.append(item)
        # The bundles naming what goes are rewritten from derb's dumps: each
        # index keeps only the locales left, with the nofallback flag the
        # dump loses (without it ICU would look for a parent), and the break
        # rules' root forgets the dropped dictionaries and rules.
        dropped=[item[len('brkitr/'):] for item in ICU_DROPPED_ITEMS]
        rewritten=indexes+['brkitr/root.res']
        (work/'rewritten.lst').write_text('\n'.join(rewritten)+'\n')
        tool('icupkg','-x','rewritten.lst','-d','bundles',data)
        for bundle in rewritten:
            folder=work/'bundles'/bundle[:bundle.rfind('/')+1];name=bundle[bundle.rfind('/')+1:-len('.res')]
            tool('derb','-s',folder,'-d',folder,name+'.res')
            text=(folder/(name+'.txt')).read_text()
            if name=='res_index':
                text=text.replace('res_index{','res_index:table(nofallback){',1)
                lines=[line for line in text.splitlines() if not (
                    m:=re.fullmatch(r'\s+([A-Za-z0-9_]+) \{ "" \}',line)) or m.group(1) in ICU_LOCALES]
            else:
                lines=[line for line in text.splitlines() if not any(item in line for item in dropped)]
            (folder/(name+'.txt')).write_text('\n'.join(lines)+'\n')
            tool('genrb','-s',folder,'-d',folder,name+'.txt')
        # One pass: the rewritten bundles replace their originals, whose
        # dependents would otherwise be orphaned between removal and adding.
        (work/'removed.lst').write_text('\n'.join(removed)+'\n')
        tool('icupkg','-r','removed.lst','-a','rewritten.lst','-s','bundles',data,data.name)
        # An assembly listing of the data, which needs neither ICU's headers
        # nor the C library's to build.
        tool('genccode','-a','gcc','-e',entry,'-d',work,data.name)
        source=work/(data.stem+'_dat.S');object_file=source.with_suffix('.o')
        subprocess.run([arch.tool('gcc'),*CPU,'-c',str(source),'-o',str(object_file)],check=True)
        subprocess.run([arch.tool('ar'),'rcsD',str(library),str(object_file)],check=True)
        stamp.write_text(signature+'\n')
    shutil.copy2(library,SYSROOT/'lib/libicudata.a')
    print('Native ICU data, trimmed:',(work/data.name).stat().st_size,'bytes',flush=True)


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
    env.update(CC=arch.tool('gcc'),CXX=arch.tool('g++'),AR=arch.tool('ar'),RANLIB=arch.tool('ranlib'),icu_cv_host_frag='mh-sos2',
        CFLAGS=shlex.join(flags),CXXFLAGS=shlex.join(flags+['-I'+str(includes),'-I'+str(includes/arch.TRIPLE)]),
        LDFLAGS='-nostartfiles -Wl,-T,'+str(arch.LINKER_SCRIPT)+' -L'+str(SYSROOT/'lib')+' -L'+str(BASE/'sdk')+
                (' -Wl,'+','.join(arch.LINK) if arch.LINK else ''),
        LIBS='-Wl,--whole-archive -lsos2 -Wl,--no-whole-archive -Wl,--start-group -lstdc++ -lc -lm -lgcc -Wl,--end-group')
    options=[str(source/'configure'),'--build=x86_64-pc-linux-gnu','--host='+arch.TRIPLE,
        '--prefix='+str(SYSROOT),'--with-cross-build='+str(host),'--with-data-packaging=static','--disable-tools',*common]
    signature=hashlib.sha256(json.dumps([options,flags,arch.prefix(),env['LDFLAGS'],'mh-sos2',digest(ROOT/'sdk/include/sos2/abi.h')]).encode()).hexdigest();stamp=build/'.sos2-config'
    if not stamp.exists() or stamp.read_text().strip()!=signature:
        if stamp.exists():shutil.rmtree(build)
        run(options,build,'icu-configure',env);stamp.write_text(signature+'\n')
    run(['make','-j'+str(jobs)],build,'icu-build',env)
    with preserve_headers():run(['make','install'],build,'icu-install',env)
    trim_icu_data()
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
