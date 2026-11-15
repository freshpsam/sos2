#!/usr/bin/env python3
"""Build the real WebKit engine against the native SOS2 SDK, for the browser."""
import argparse
import hashlib
import json
import os
import subprocess
from build_native_runtime import ROOT, SYSROOT
from host_tools import find_tool
import build_native_mesa as mesa
from native_sources import apply_patch, copy_if_changed, digest, download, run_logged, source_tree

BASE=ROOT/'build/native-webkit'
RECORD=json.loads((ROOT/'tools/browser/native-webkit.lock.json').read_text())
SOURCE=BASE/'src'/('wpewebkit-'+RECORD['version'])
ARCHIVE=BASE/'downloads'/RECORD['url'].rsplit('/',1)[1]


def fetch():
    download(RECORD['url'],ARCHIVE,RECORD['sha256'])


def prepare():
    suffixes=ROOT/'third_party/publicsuffix'
    manifest=json.loads((suffixes/'manifest.json').read_text())
    for filename,key in (('public_suffix_list.dat','source_sha256'),('rules.inc','rules_sha256')):
        if digest(suffixes/filename)!=manifest[key]:raise RuntimeError('Public Suffix List checksum mismatch: '+filename)
    source_tree(ARCHIVE,SOURCE,RECORD['sha256'])
    for name, marker in (('sos2', '.sos2-port'), ('input', '.sos2-input-port'),
                         ('network', '.sos2-network-port'), ('clock', '.sos2-clock-port'),
                         ('wait', '.sos2-wait-port'), ('fonts', '.sos2-font-port'),
                         ('rendering', '.sos2-rendering-port'), ('cores', '.sos2-cores-port'),
                         ('workers', '.sos2-workers-port'), ('neon', '.sos2-neon-port'),
                         ('scrolling', '.sos2-scrolling-port'),
                         ('filter-bounds', '.sos2-filter-bounds-port'),
                         ('llint', '.sos2-llint-port'),
                         ('compositing', '.sos2-compositing-port'),
                         ('gpu-compositing', '.sos2-gpu-compositing-port')):
        apply_patch(SOURCE, ROOT / 'tools/browser/patches' / ('wpe-2.54.2-native-' + name + '.patch'), marker,
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
    options={
        'PORT':'SOS2','CMAKE_BUILD_TYPE':'Release','ENABLE_JIT':'OFF','ENABLE_DFG_JIT':'OFF',
        'ENABLE_FTL_JIT':'OFF','ENABLE_C_LOOP':'OFF','ENABLE_WEBASSEMBLY':'OFF','ENABLE_SAMPLING_PROFILER':'OFF',
        'USE_SYSTEM_MALLOC':'OFF','USE_MIMALLOC':'ON','USE_ISO_MALLOC':'OFF','USE_CAPSTONE':'OFF',
        'ENABLE_STATIC_JSC':'ON','ENABLE_REMOTE_INSPECTOR':'OFF','ENABLE_TEST_SUPPORT':'OFF',
        'EVENT_LOOP_TYPE':'generic','USE_LIBBACKTRACE':'OFF','USE_GLIB':'OFF','ENABLE_JSC_GLIB_API':'OFF',
    }
    options.update(USE_SKIA='ON',USE_SKIA_OPENTYPE_SVG='OFF',ENABLE_VIDEO='OFF',ENABLE_WEB_AUDIO='OFF',
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
    signature=hashlib.sha256((json.dumps(options)+digest(ROOT/'sdk/sos2-arm32.cmake')).encode()).hexdigest()
    destination=BASE/'webcore-build'
    stamp=destination/'.sos2-config';fresh=[] if stamp.exists() and stamp.read_text().strip()==signature else ['--fresh']
    run(['cmake',*fresh,'-S',str(SOURCE),'-B',str(destination),'-G','Ninja',
        '-DCMAKE_TOOLCHAIN_FILE='+str(ROOT/'sdk/sos2-arm32.cmake'),
        *['-D'+key+'='+value for key,value in options.items()]],'webcore-configure')
    stamp.write_text(signature+'\n')


def pack_web(output=None,trust_store=None):
    from runtime_bundle import bundle
    from build_native_render import RECORDS,prepare as prepare_dependency
    from browser_fonts import resources as font_resources
    fonts=prepare_dependency(next(x for x in RECORDS if x['name']=='fonts'))
    linked=BASE/'webcore-build/bin/sos2-web.elf'
    executable=BASE/'sos2-web.elf'
    subprocess.run(['arm-none-eabi-strip','--strip-unneeded','-o',str(executable),str(linked)],check=True)
    # Web documents and assets live on the private writable disk, never here.
    files={'/bin/sos2-web':executable,'/fonts/LICENSE':fonts/'LICENSE'}
    roots=ROOT/'third_party/ca-certificates/mozilla.pem'
    if digest(roots)!='427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285':raise RuntimeError('Unexpected Mozilla trust bundle digest')
    # Test fixtures may explicitly build a separate image with a fixture CA.
    # Web content and runtime configuration cannot change trust anchors.
    files['/certs/roots.pem']=trust_store or roots
    for family,slant in (('DejaVuSans','Oblique'),('DejaVuSerif','Italic'),('DejaVuSansMono','Oblique')):
        for style in ('','-Bold','-'+slant,'-Bold'+slant):
            name=family+style+'.ttf';files['/fonts/'+name]=fonts/'ttf'/name
    files.update(font_resources())
    from build_native_apps import files as app_files
    files.update(app_files())
    destination=output or BASE/'native-web.bundle'
    bundle(destination,files)
    print('Native WebKit bundle:',destination,flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('command',choices=('fetch','prepare','configure','build','bundle'))
    parser.add_argument('-j','--jobs',type=int,default=6);args=parser.parse_args()
    try:
        if args.command=='fetch':fetch()
        elif args.command=='prepare':prepare()
        elif args.command=='configure':configure()
        elif args.command=='bundle':pack_web()
        else:
            mesa.build(args.jobs);configure()
            run(['cmake','--build',str(BASE/'webcore-build'),'--target','sos2-web','-j',str(args.jobs)],'webcore-build')
            pack_web()
    except (OSError,RuntimeError) as error:parser.exit(1,str(error)+'\n')


if __name__=='__main__':main()
