#!/usr/bin/env python3
"""Source-build WebCore's portable dependencies for the native SOS2 ABI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tarfile
import zipfile
import arch
from build_native_runtime import ROOT,SYSROOT,sdk_flags,preserve_headers
from native_sources import apply_patch,copy_if_changed,digest,download,run_logged

BASE=ROOT/('build/native-webkit'+arch.SUFFIX)
# Pinned archives are the same for every architecture: make fetch fills ARM32's.
DOWNLOADS=ROOT/'build/native-webkit/downloads'
RECORDS=json.loads((ROOT/'tools/browser/native-render.lock.json').read_text())['sources']


def fetch():
    for record in RECORDS:download(record['url'],DOWNLOADS/record['archive'],record['sha256'])
    from browser_fonts import fetch as fetch_fonts
    fetch_fonts()


def prepare(record):
    archive=DOWNLOADS/record['archive']
    directory=record['archive'].removesuffix('.tar.bz2') if record['name']=='mbedtls' else record['name']
    destination=BASE/'dependencies/src'/directory
    stamp=destination/'.sos2-source'
    if stamp.exists() and stamp.read_text().strip()==record['sha256']:return destination
    if destination.exists():raise RuntimeError('Unrecognized source directory: '+str(destination))
    if not archive.exists() or digest(archive)!=record['sha256']:raise RuntimeError('Fetch pinned source: '+record['name'])
    temporary=destination.with_name(record['name']+'-extract');temporary.mkdir(parents=True,exist_ok=False)
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as source:
            for member in source.namelist():
                if Path(member).is_absolute() or '..' in Path(member).parts:raise RuntimeError('Unsafe archive path')
            source.extractall(temporary)
    else:
        with tarfile.open(archive) as source:source.extractall(temporary,filter='data')
    children=list(temporary.iterdir())
    if len(children)!=1 or not children[0].is_dir():raise RuntimeError('Expected one source directory')
    children[0].rename(destination);temporary.rmdir();stamp.write_text(record['sha256']+'\n')
    return destination


def run(command,label,cwd=None):
    env=os.environ.copy();env['PKG_CONFIG_LIBDIR']=str(SYSROOT/'lib/pkgconfig');env['PKG_CONFIG_PATH']='';env['PKG_CONFIG_SYSROOT_DIR']=str(SYSROOT)
    run_logged(command,BASE/'dependencies'/(label+'.log'),'Native dependency',cwd,env)


def static_library(name,sources,includes=(),definitions=()):
    build=BASE/'dependencies'/name;build.mkdir(parents=True,exist_ok=True);objects=[]
    for source in sources:
        output=build/(source.name+'.o');objects.append(str(output))
        run([arch.tool('gcc'),*sdk_flags(),*['-I'+str(x) for x in includes],*['-D'+x for x in definitions],
             '-c',str(source),'-o',str(output)],name+'-'+source.name)
    run([arch.tool('ar'),'rcs',str(SYSROOT/'lib'/('lib'+name+'.a')),*objects],name+'-archive')


def patch_xml_encoding(source):
    # ARM EABI uses small enums. libxml2 2.14's new enum-returning converters
    # still returned byte counts, truncating successful output >=128 bytes.
    # Keep the native ABI; return status codes and preserve legacy int counts.
    apply_patch(source,ROOT/'tools/browser/patches/libxml2-2.14.4-sos2-encoding.patch','.sos2-encoding-patch','XML encoding')


def build(record,jobs):
    name=record['name'];source=prepare(record)
    output=BASE/'dependencies'/((source.name if name=='mbedtls' else name)+'-build')
    if name=='xml2' and arch.SHORT_ENUMS:patch_xml_encoding(source)
    if name=='fonts':return
    if name=='zlib':
        files='adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil'.split()
        static_library('z',[source/(x+'.c') for x in files],[source],['HAVE_UNISTD_H'])
        for header in ('zlib.h','zconf.h'):copy_if_changed(source/header,SYSROOT/'include'/header)
        return
    if name=='sqlite':
        amalgamation=source/'sqlite3.c';text=amalgamation.read_text()
        old='#  if SQLITE_OS_UNIX\n#    define SQLITE_MUTEX_PTHREADS'
        new='#  if SQLITE_OS_UNIX || defined(SQLITE_MUTEX_PTHREADS)\n#    define SQLITE_MUTEX_PTHREADS'
        if text.count(old)==1:amalgamation.write_text(text.replace(old,new))
        elif text.count(new)!=1:raise RuntimeError('Unrecognized SQLite mutex selection')
        static_library('sqlite3',[source/'sqlite3.c',ROOT/'sdk/runtime/sqlite_vfs.c'],[source],
            ['SQLITE_OS_OTHER=1','SQLITE_THREADSAFE=1','SQLITE_MUTEX_PTHREADS=1','SQLITE_OMIT_LOAD_EXTENSION=1',
             'SQLITE_MAX_MMAP_SIZE=0','SQLITE_TEMP_STORE=3','SQLITE_DEFAULT_MEMSTATUS=0'])
        for header in ('sqlite3.h','sqlite3ext.h'):copy_if_changed(source/header,SYSROOT/'include'/header)
        return
    options={
        'png':{'PNG_SHARED':'OFF','PNG_STATIC':'ON','PNG_TESTS':'OFF','PNG_TOOLS':'OFF','PNG_ARM_NEON':'off'},
        'jpeg':{'ENABLE_SHARED':'OFF','ENABLE_STATIC':'ON','WITH_SIMD':'OFF','WITH_TURBOJPEG':'OFF'},
        'freetype':{'FT_DISABLE_BZIP2':'ON','FT_DISABLE_BROTLI':'ON','FT_DISABLE_HARFBUZZ':'ON'},
        'harfbuzz':{'HB_HAVE_ICU':'ON','HB_HAVE_FREETYPE':'ON','HB_HAVE_GLIB':'OFF','HB_HAVE_GOBJECT':'OFF','HB_BUILD_UTILS':'OFF','HB_BUILD_SUBSET':'OFF'},
        'webp':{'WEBP_BUILD_ANIM_UTILS':'OFF','WEBP_BUILD_CWEBP':'OFF','WEBP_BUILD_DWEBP':'OFF','WEBP_BUILD_GIF2WEBP':'OFF','WEBP_BUILD_IMG2WEBP':'OFF','WEBP_BUILD_VWEBP':'OFF','WEBP_BUILD_WEBPINFO':'OFF','WEBP_BUILD_WEBPMUX':'OFF','WEBP_BUILD_EXTRAS':'OFF','WEBP_USE_THREAD':'OFF'},
        'xml2':{'LIBXML2_WITH_ICONV':'OFF','LIBXML2_WITH_LZMA':'OFF','LIBXML2_WITH_PYTHON':'OFF','LIBXML2_WITH_TESTS':'OFF','LIBXML2_WITH_PROGRAMS':'OFF','LIBXML2_WITH_HTTP':'OFF','LIBXML2_WITH_FTP':'OFF','CMAKE_HAVE_LIBC_PTHREAD':'1'},
        'mbedtls':{'ENABLE_PROGRAMS':'OFF','ENABLE_TESTING':'OFF','USE_SHARED_MBEDTLS_LIBRARY':'OFF','USE_STATIC_MBEDTLS_LIBRARY':'ON',
                   'MBEDTLS_CONFIG_FILE':str(ROOT/'tools/browser/mbedtls-sos2.h')},
        'nghttp2':{'ENABLE_LIB_ONLY':'ON','BUILD_STATIC_LIBS':'ON',
                   'ENABLE_DOC':'OFF','BUILD_TESTING':'OFF','ENABLE_HTTP3':'OFF'},
    }[name]
    configure=['-S',str(source),'-B',str(output),'-G','Ninja',
         '-DCMAKE_TOOLCHAIN_FILE='+str(arch.CMAKE_TOOLCHAIN),'-DCMAKE_BUILD_TYPE=Release',
         '-DCMAKE_INSTALL_PREFIX='+str(SYSROOT),'-DCMAKE_INSTALL_LIBDIR=lib','-DBUILD_SHARED_LIBS=OFF',
         *['-D'+key+'='+value for key,value in options.items()]]
    # CMake keeps the first configure's toolchain flags in its cache. Start
    # fresh whenever these options, the compiler prefix or the SDK toolchain
    # file change.
    signature=hashlib.sha256((json.dumps([configure,arch.prefix()])+digest(arch.CMAKE_TOOLCHAIN)).encode()).hexdigest()
    stamp=output/'.sos2-config'
    fresh=[] if stamp.exists() and stamp.read_text().strip()==signature else ['--fresh']
    run(['cmake',*fresh,*configure],name+'-configure')
    stamp.write_text(signature+'\n')
    run(['cmake','--build',str(output),'-j',str(jobs)],name+'-build')
    with preserve_headers():run(['cmake','--install',str(output)],name+'-install')
    if name=='png':
        # libpng's Generic CMake target uses this non-Unix archive basename.
        # Supply the conventional name used by FreeType/WebKit's FindPNG.
        library=SYSROOT/'lib/liblibpng16_static.a';alias=SYSROOT/'lib/libpng.a'
        if not library.is_file():raise RuntimeError('Missing native PNG archive')
        if alias.exists() or alias.is_symlink():
            if alias.resolve()!=library.resolve():raise RuntimeError('Unexpected PNG library alias: '+str(alias))
        else:alias.symlink_to(library.name)
    if name=='mbedtls':
        (SYSROOT/'include/sos2').mkdir(exist_ok=True)
        copy_if_changed(ROOT/'tools/browser/mbedtls-sos2.h',SYSROOT/'include/sos2/mbedtls_config.h')


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('command',choices=('fetch','build'))
    parser.add_argument('-j','--jobs',type=int,default=6)
    args=parser.parse_args()
    try:
        if args.command=='fetch':fetch()
        else:
            for record in RECORDS:build(record,args.jobs)
    except (OSError,RuntimeError) as error:parser.exit(1,str(error)+'\n')


if __name__=='__main__':main()
