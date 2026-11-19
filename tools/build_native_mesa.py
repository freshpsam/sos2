#!/usr/bin/env python3
"""Build Mesa's EGL, OpenGL ES 2 and virgl Gallium driver as static archives
for SOS2 pages, with the libdrm shim (sdk/drm) that serves Mesa's virtio-gpu
render node over SYS_GPU."""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import arch
from build_native_runtime import ROOT, SYSROOT, CPU, link_flags, link_libraries, sdk, sdk_includes
from host_tools import find_tool
from native_sources import apply_patch, digest, download, run_logged, source_tree

BASE = ROOT/('build/native-mesa'+arch.SUFFIX)
RECORDS = {record['name']: record for record in
           json.loads((ROOT/'tools/browser/native-mesa.lock.json').read_text())['sources']}
MESA = BASE/'src'/('mesa-'+RECORDS['mesa']['version'])
LIBDRM = BASE/'src'/('libdrm-'+RECORDS['libdrm']['version'])
PATCH = ROOT/'tools/browser/patches/mesa-26.2.0-sos2.patch'
BUILD = BASE/'build'
SHIM = BASE/'drm-shim'
OPTIONS = [
    '--buildtype=release', '-Ddefault_library=static', '-Db_staticpic=false',
    '-Dplatforms=', '-Degl-native-platform=surfaceless', '-Degl=enabled', '-Dgles1=disabled', '-Dgles2=enabled',
    '-Dopengl=false', '-Dglx=disabled', '-Dgbm=disabled', '-Dglvnd=disabled', '-Dllvm=disabled',
    '-Dgallium-drivers=virgl', '-Dvulkan-drivers=', '-Dvideo-codecs=', '-Dgallium-va=disabled',
    '-Dgallium-rusticl=false', '-Dteflon=false', '-Dmicrosoft-clc=disabled', '-Dmesa-clc=auto',
    '-Dspirv-tools=disabled', '-Dshader-cache=disabled', '-Dzlib=disabled', '-Dzstd=disabled',
    '-Dexpat=disabled', '-Dxmlconfig=disabled', '-Dvalgrind=disabled', '-Dlibunwind=disabled',
    '-Dlmsensors=disabled', '-Dandroid-libbacktrace=disabled', '-Dperfetto=false', '-Dbuild-tests=false',
    '-Dtools=', '-Dhtml-docs=disabled',
]


def compile_flags(cxx=False):
    """Flags for Mesa and the shim. arm-none-eabi defaults to short enums,
    which Mesa's enum bitfields do not fit; the GL and EGL interfaces pass no
    enum types, so programs may mix either size."""
    return [*CPU, *(['-fno-short-enums'] if arch.SHORT_ENUMS else []), '-DEGL_NO_PLATFORM_SPECIFIC_TYPES',
            '-ffunction-sections', '-fdata-sections', *sdk_includes(cxx)]


def libraries():
    """Static archives a GLES program links, inside one group."""
    return [BUILD/'src/egl/libEGL.a', BUILD/'src/mesa/glapi/es2api/libGLESv2.a',
            BUILD/'src/gallium/targets/dri'/('libgallium-'+RECORDS['mesa']['version']+'.a'), SHIM/'libdrm.a']


def include_directories():
    """EGL and GLES headers, and the shim's drm_status.h."""
    return [MESA/'include', ROOT/'sdk/drm']


def archive(record):
    return ROOT/'build/native-mesa/downloads'/record['url'].rsplit('/', 1)[1]


def fetch():
    for record in RECORDS.values():
        download(record['url'], archive(record), record['sha256'])


def prepare():
    for record, folder in ((RECORDS['mesa'], MESA), (RECORDS['libdrm'], LIBDRM)):
        source_tree(archive(record), folder, record['sha256'])
    apply_patch(MESA, PATCH, '.sos2-patch', 'Mesa')


def run(command, label, env=None):
    run_logged(command, BASE/(label+'.log'), 'Native Mesa', env=env)


def write_if_changed(path, text):
    if not path.is_file() or path.read_text() != text:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)


def toolchain():
    """Compiler wrappers, the shim's pkg-config file and the meson cross file."""
    # Bare-metal GCC rejects -pthread, which meson adds for threads; SOS2
    # programs always have threads.
    for name in ('gcc', 'g++'):
        wrapper = BASE/'bin'/(arch.TRIPLE+'-'+name)
        write_if_changed(wrapper, '#!/bin/sh\nfor argument do shift; [ "$argument" = -pthread ] || '
                         'set -- "$@" "$argument"; done\nexec '+arch.tool(name)+' "$@"\n')
        wrapper.chmod(0o755)
    version = RECORDS['libdrm']['version']
    write_if_changed(BASE/'pkgconfig/libdrm.pc',
                     'Name: libdrm\nDescription: SOS2 libdrm shim over SYS_GPU (sdk/drm)\nVersion: '+version+'\n'
                     'Cflags: -I'+str(LIBDRM)+' -I'+str(LIBDRM/'include/drm')+'\nLibs: -L'+str(SHIM)+' -ldrm\n')

    def array(values):
        return '['+', '.join("'"+str(v).replace('\\', '\\\\').replace("'", "\\'")+"'" for v in values)+']'
    # Mesa's 32-bit enums (above) would draw a warning per object mixed with SDK code.
    linking = link_flags(*(['--no-enum-size-warning'] if arch.SHORT_ENUMS else []))
    cross = BASE/'cross-sos2.ini'
    write_if_changed(cross, '\n'.join([
        '[binaries]', 'c = '+array([BASE/'bin'/(arch.TRIPLE+'-gcc')])[1:-1],
        'cpp = '+array([BASE/'bin'/(arch.TRIPLE+'-g++')])[1:-1],
        'ar = '+array([arch.tool('ar')])[1:-1], 'strip = '+array([arch.tool('strip')])[1:-1], "pkg-config = 'pkg-config'", '',
        '[properties]', 'needs_exe_wrapper = true', 'pkg_config_libdir = '+array([BASE/'pkgconfig'])[1:-1],
        'sys_root = '+array([SYSROOT])[1:-1], '',
        '[built-in options]', 'c_args = '+array(compile_flags()), 'cpp_args = '+array(compile_flags(True)),
        'c_link_args = '+array(linking+link_libraries()), 'cpp_link_args = '+array(linking+link_libraries(True)), '',
        '[host_machine]', "system = 'sos2'", "cpu_family = '"+arch.MESON_CPU[0]+"'", "cpu = '"+arch.MESON_CPU[1]+"'",
        "endian = 'little'", '']))
    return cross


def shim():
    SHIM.mkdir(parents=True, exist_ok=True)
    run([arch.tool('gcc'), *compile_flags(), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
         '-I'+str(LIBDRM), '-I'+str(LIBDRM/'include/drm'), '-I'+str(MESA/'src/virtio/virtio-gpu'),
         '-c', ROOT/'sdk/drm/drm.c', '-o', SHIM/'drm.o'], 'drm-shim')
    (SHIM/'libdrm.a').unlink(missing_ok=True)
    run([arch.tool('ar'), 'rcs', SHIM/'libdrm.a', SHIM/'drm.o'], 'drm-shim-archive')


def environment():
    """Meson, Mako, flex and bison from the host or from .tools/ copies."""
    env = os.environ.copy()
    paths, python_paths = [], []
    meson, meson_root = find_tool('MESON', 'meson', 'mesa')
    if not meson or not shutil.which('ninja'):
        raise RuntimeError('Install the host build dependencies: meson and ninja')
    if meson_root:
        python_paths.append(meson_root/'usr/lib/python3/dist-packages')
    for variable, executable, packages in (('FLEX', 'flex', ('mesa',)), ('BISON', 'bison', ('mesa', 'webkit'))):
        for package in packages:
            tool, root = find_tool(variable, executable, package)
            if tool:
                break
        if not tool:
            raise RuntimeError('Install the host build dependency: '+executable)
        if root:
            paths.append(root/'usr/bin')
            if executable == 'bison':
                env['BISON_PKGDATADIR'] = str(root/'usr/share/bison')
    if subprocess.run([sys.executable, '-c', 'import mako'], capture_output=True).returncode:
        mako = ROOT/'.tools/mesa/usr/lib/python3/dist-packages'
        if not (mako/'mako').is_dir():
            raise RuntimeError('Install the host build dependency: python3-mako')
        python_paths.append(mako)
    env['PATH'] = os.pathsep.join([*map(str, paths), env.get('PATH', '')])
    if python_paths:
        env['PYTHONPATH'] = os.pathsep.join([*dict.fromkeys(map(str, python_paths)), *filter(None, [env.get('PYTHONPATH')])])
    return ([sys.executable, meson] if meson_root else [meson]), env


def build(jobs):
    sdk()
    prepare()
    cross = toolchain()
    shim()
    meson, env = environment()
    signature = hashlib.sha256(json.dumps([OPTIONS, cross.read_text(), digest(PATCH)]).encode()).hexdigest()
    stamp = BUILD/'.sos2-config'
    if not stamp.exists() or stamp.read_text().strip() != signature:
        shutil.rmtree(BUILD, ignore_errors=True)
        run([*meson, 'setup', BUILD, MESA, '--cross-file', cross, *OPTIONS], 'configure', env)
        stamp.write_text(signature+'\n')
    run(['ninja', '-C', BUILD, '-j', str(jobs)], 'build', env)
    for library in libraries():
        if not library.is_file():
            raise RuntimeError('Mesa build did not produce '+str(library))
    print('Native Mesa:', BUILD, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__ + ' make engine builds it; this fetches its sources.')
    parser.add_argument('command', choices=('fetch',))
    parser.parse_args()
    try:
        fetch()
    except (OSError, RuntimeError) as error:
        parser.exit(1, str(error)+'\n')


if __name__ == '__main__':
    main()
