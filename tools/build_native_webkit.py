#!/usr/bin/env python3
"""Build the real WebKit engine against the native SOS2 SDK, for the browser."""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path
import arch
from build_native_runtime import ROOT, SYSROOT
from host_tools import find_tool
import build_native_mesa as mesa
from native_sources import apply_patch, copy_if_changed, digest, download, run_logged, source_tree

BASE=ROOT/('build/native-webkit'+arch.SUFFIX)
RECORD=json.loads((ROOT/'tools/browser/native-webkit.lock.json').read_text())
SOURCE=BASE/'src'/('wpewebkit-'+RECORD['version'])
ARCHIVE=ROOT/'build/native-webkit/downloads'/RECORD['url'].rsplit('/',1)[1]
# WPE 2.52.6 vendors no mimalloc; SOS2 takes the one WPE 2.54.2 vendors.
MIMALLOC=RECORD['mimalloc']
MIMALLOC_ARCHIVE=ROOT/'build/native-webkit/downloads'/MIMALLOC['url'].rsplit('/',1)[1]


def fetch():
    download(RECORD['url'],ARCHIVE,RECORD['sha256'])
    download(MIMALLOC['url'],MIMALLOC_ARCHIVE,MIMALLOC['sha256'])


def vendor_mimalloc():
    """Publish the pinned mimalloc sources and their stamp in one rename."""
    target = SOURCE/'Source/bmalloc/mimalloc'
    expected = MIMALLOC['sha256']
    stamp = target/'.sos2-source'
    if stamp.is_file():
        if stamp.read_text().strip() != expected:
            raise RuntimeError('Mismatched mimalloc source stamp: '+str(target))
        return
    if target.exists() or target.is_symlink():
        raise RuntimeError('Unstamped mimalloc source: '+str(target))
    if not MIMALLOC_ARCHIVE.is_file() or digest(MIMALLOC_ARCHIVE) != expected:
        raise RuntimeError('Missing/mismatched source: '+str(MIMALLOC_ARCHIVE)+'; run make fetch')
    prefix = MIMALLOC['path']+'/'
    with tempfile.TemporaryDirectory(prefix='.mimalloc-', dir=target.parent) as tmp:
        tree = Path(tmp)/'mimalloc'
        with tarfile.open(MIMALLOC_ARCHIVE) as source:
            members = [member for member in source.getmembers() if member.name.startswith(prefix)]
            if not members:
                raise RuntimeError('No '+MIMALLOC['path']+' in '+str(MIMALLOC_ARCHIVE))
            for member in members:
                member.name = member.name[len(prefix):]
            source.extractall(tree, members=members, filter='data')
        (tree/'.sos2-source').write_text(expected+'\n')
        tree.rename(target)


def prepare():
    suffixes=ROOT/'third_party/publicsuffix'
    manifest=json.loads((suffixes/'manifest.json').read_text())
    for filename,key in (('public_suffix_list.dat','source_sha256'),('rules.inc','rules_sha256')):
        if digest(suffixes/filename)!=manifest[key]:raise RuntimeError('Public Suffix List checksum mismatch: '+filename)
    source_tree(ARCHIVE,SOURCE,RECORD['sha256'])
    vendor_mimalloc()
    for name, marker in (('sos2', '.sos2-port'), ('input', '.sos2-input-port'),
                         ('network', '.sos2-network-port'), ('clock', '.sos2-clock-port'),
                         ('wait', '.sos2-wait-port'), ('fonts', '.sos2-font-port'),
                         ('rendering', '.sos2-rendering-port'), ('cores', '.sos2-cores-port'),
                         ('workers', '.sos2-workers-port'), ('neon', '.sos2-neon-port'),
                         ('scrolling', '.sos2-scrolling-port'),
                         ('filter-bounds', '.sos2-filter-bounds-port'),
                         ('llint', '.sos2-llint-port'),
                         ('compositing', '.sos2-compositing-port'),
                         ('gpu-compositing', '.sos2-gpu-compositing-port'),
                         ('fixed-layers', '.sos2-fixed-layers-port'),
                         ('blend-flush', '.sos2-blend-flush-port'),
                         ('mimalloc', '.sos2-mimalloc-port'),
                         ('blocks', '.sos2-blocks-port'),
                         ('media', '.sos2-media-port'),
                         ('image-decoders', '.sos2-image-decoders-port'),
                         ('dom-lifetimes', '.sos2-dom-lifetimes-port'),
                         ('image-loader', '.sos2-image-loader-port'),
                         ('jsc-host-call', '.sos2-jsc-host-call-port'),
                         ('jsc-bytecode-metadata', '.sos2-jsc-bytecode-metadata-port'),
                         ('jsc-dfg', '.sos2-jsc-dfg-port'),
                         ('yarr-unicode', '.sos2-yarr-unicode-port'),
                         ('svg-repeat', '.sos2-svg-repeat-port'),
                         # WebKit 2.54's universal pseudo-element rule bucket.
                         ('rules', '.sos2-rules-port'),
                         # Each architecture's JIT glue.
                         *((('arm64', '.sos2-arm64-port'),) if arch.ARM64 else (('jit', '.sos2-jit-port'),))):
        apply_patch(SOURCE, ROOT / 'tools/browser/patches' / ('wpe-' + RECORD['version'] + '-native-' + name + '.patch'), marker,
                    'WebKit ' + name)
    overlay=ROOT/'tools/browser/native-port'
    for path in overlay.rglob('*'):
        if not path.is_file():continue
        target=SOURCE/path.relative_to(overlay);target.parent.mkdir(parents=True,exist_ok=True)
        copy_if_changed(path,target)
    print('Native WebKit source:',SOURCE,flush=True)


def run(command,label):
    env=os.environ.copy();env['PKG_CONFIG_LIBDIR']=str(SYSROOT/'lib/pkgconfig');env['PKG_CONFIG_PATH']=''
    env['PKG_CONFIG_SYSROOT_DIR']=str(SYSROOT)
    _,host=find_tool('BISON','bison','webkit')
    if host:
        env['BISON_PKGDATADIR']=str(host/'usr/share/bison')
    # Optional already-provisioned host Ruby, solely for WebKit code generation.
    # It is never a target include/library search root.
    _,rubyroot=find_tool('RUBY','ruby','webkit')
    if rubyroot:
        env['LD_LIBRARY_PATH']=os.pathsep.join(filter(None,(str(rubyroot/'usr/lib/x86_64-linux-gnu'),env.get('LD_LIBRARY_PATH'))))
        env['RUBYLIB']=os.pathsep.join(str(rubyroot/path) for path in ('usr/lib/ruby/3.3.0','usr/lib/x86_64-linux-gnu/ruby/3.3.0'))
        env['RUBYOPT']='--disable-gems'
    run_logged(command,BASE/(label+'.log'),'Native WebKit',env=env)


def configure():
    prepare()
    # Both architectures compile JavaScriptCore's baseline and DFG tiers;
    # sos2.jsc= picks them at run time (programs/browser/web_main.cpp). FTL's
    # B3 backend needs WebAssembly in this WebKit.
    # -O2 rather than Release's -O3: smaller code, so that the system fits a
    # 32 MiB boot partition on both devices.
    options={
        'PORT':'SOS2','CMAKE_BUILD_TYPE':'Release','CMAKE_C_FLAGS_RELEASE':'-O2 -DNDEBUG',
        'CMAKE_CXX_FLAGS_RELEASE':'-O2 -DNDEBUG','ENABLE_JIT':'ON','ENABLE_DFG_JIT':'ON',
        'ENABLE_FTL_JIT':'OFF','ENABLE_C_LOOP':'OFF','ENABLE_WEBASSEMBLY':'OFF','ENABLE_SAMPLING_PROFILER':'OFF',
        'USE_SYSTEM_MALLOC':'OFF','USE_MIMALLOC':'ON','USE_ISO_MALLOC':'OFF','USE_CAPSTONE':'OFF',
        'ENABLE_STATIC_JSC':'ON','ENABLE_REMOTE_INSPECTOR':'OFF','ENABLE_TEST_SUPPORT':'OFF',
        'EVENT_LOOP_TYPE':'generic','USE_LIBBACKTRACE':'OFF','USE_GLIB':'OFF','ENABLE_JSC_GLIB_API':'OFF',
    }
    options.update(USE_SKIA='ON',USE_SKIA_OPENTYPE_SVG='OFF',ENABLE_VIDEO='ON',ENABLE_MEDIA_SOURCE='ON',ENABLE_WEB_AUDIO='OFF',
        ENABLE_WEB_RTC='OFF',ENABLE_MEDIA_STREAM='OFF',ENABLE_SPEECH_SYNTHESIS='OFF',ENABLE_SPELLCHECK='OFF',
        ENABLE_XSLT='OFF',ENABLE_GAMEPAD='OFF',ENABLE_WEBGL='OFF',ENABLE_WEBGPU='OFF',ENABLE_GPU_PROCESS='OFF',
        ENABLE_ASYNC_SCROLLING='OFF',ENABLE_OFFSCREEN_CANVAS='OFF',ENABLE_OFFSCREEN_CANVAS_IN_WORKERS='OFF',
        ENABLE_WEB_CODECS='OFF',ENABLE_NOTIFICATIONS='OFF',ENABLE_PERIODIC_MEMORY_MONITOR='OFF',ENABLE_RESOURCE_USAGE='OFF',
        USE_LIBHYPHEN='OFF',USE_AVIF='OFF',USE_JPEGXL='OFF',USE_LCMS='OFF',USE_WOFF2='OFF')
    for variable,executable,option in (('BISON','bison','BISON_EXECUTABLE'),
            ('GPERF','gperf','GPERF_EXECUTABLE'),('RUBY','ruby','Ruby_EXECUTABLE')):
        tool,_=find_tool(variable,executable,'webkit')
        if not tool:raise RuntimeError('Install the host build dependency: '+executable)
        options[option]=tool
    unifdef,_=find_tool('UNIFDEF','unifdef','webkit')
    if unifdef:options.update(USE_SYSTEM_UNIFDEF='ON',UNIFDEF_EXECUTABLE=unifdef)
    # GPU rasterization links Mesa and the libdrm shim (programs/browser/web_gpu.h).
    options.update(SOS2_MESA_INCLUDE_DIRS=';'.join(map(str,mesa.include_directories())),
                   SOS2_MESA_LIBRARIES=';'.join(map(str,mesa.libraries())))
    signature=hashlib.sha256((json.dumps([options,arch.prefix()])+digest(arch.CMAKE_TOOLCHAIN)).encode()).hexdigest()
    destination=BASE/'webcore-build'
    # Generated header copies outlive CMake's cache, and stale ones from
    # another WebKit version get compiled: a new version starts empty.
    version=destination/'.sos2-version'
    if destination.exists() and (not version.exists() or version.read_text().strip()!=RECORD['version']):
        shutil.rmtree(destination)
    stamp=destination/'.sos2-config';fresh=[] if stamp.exists() and stamp.read_text().strip()==signature else ['--fresh']
    run(['cmake',*fresh,'-S',str(SOURCE),'-B',str(destination),'-G','Ninja',
        '-DCMAKE_TOOLCHAIN_FILE='+str(arch.CMAKE_TOOLCHAIN),
        *['-D'+key+'='+value for key,value in options.items()]],'webcore-configure')
    stamp.write_text(signature+'\n')
    version.write_text(RECORD['version']+'\n')


def pack_web():
    from runtime_bundle import bundle
    from build_native_render import RECORDS,prepare as prepare_dependency
    from browser_fonts import resources as font_resources
    fonts=prepare_dependency(next(x for x in RECORDS if x['name']=='fonts'))
    linked=BASE/'webcore-build/bin/sos2-web.elf'
    executable=BASE/'sos2-web.elf'
    subprocess.run([arch.tool('strip'),'--strip-unneeded','-o',str(executable),str(linked)],check=True)
    # Web documents and assets live on the private writable disk, never here.
    files={'/bin/sos2-web':executable,'/fonts/LICENSE':fonts/'LICENSE'}
    roots=ROOT/'third_party/ca-certificates/mozilla.pem'
    if digest(roots)!='427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285':raise RuntimeError('Unexpected Mozilla trust bundle digest')
    # Web content and runtime configuration cannot change trust anchors.
    files['/certs/roots.pem']=roots
    for family,slant in (('DejaVuSans','Oblique'),('DejaVuSerif','Italic'),('DejaVuSansMono','Oblique')):
        for style in ('','-Bold','-'+slant,'-Bold'+slant):
            name=family+style+'.ttf';files['/fonts/'+name]=fonts/'ttf'/name
    files.update(font_resources())
    from build_native_apps import files as app_files
    files.update(app_files())
    destination=BASE/'native-web.bundle'
    bundle(destination,files)
    print('Native WebKit bundle:',destination,flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('command',choices=('fetch','build'))
    parser.add_argument('-j','--jobs',type=int,default=6);args=parser.parse_args()
    try:
        if args.command=='fetch':fetch()
        else:
            mesa.build(args.jobs);configure()
            run(['cmake','--build',str(BASE/'webcore-build'),'--target','sos2-web','-j',str(args.jobs)],'webcore-build')
            pack_web()
    except (OSError,RuntimeError) as error:parser.exit(1,str(error)+'\n')


if __name__=='__main__':main()
