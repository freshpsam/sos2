"""The target architecture of the native builds: SOS2_ARCH=arm32 (the default:
QEMU's ARM machine and the Redmi 2), arm64 (QEMU's AArch64 machine and the
Galaxy Tab A7 Lite) or x64 (QEMU's microvm).

ARM32 keeps the unsuffixed build directories; arm64 and x64 build into ones
ending in -arm64 and -x64. A cross tool is found on PATH, through
<ARCH>_PREFIX, or in .tools/<triple>/bin (an unpacked Arm GNU Toolchain, or
the x86_64-elf one tools/build_x64_toolchain.py builds).
"""
import os
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
ARCH = os.environ.get('SOS2_ARCH', 'arm32')
if ARCH not in ('arm32', 'arm64', 'x64'):
    raise SystemExit(f'SOS2_ARCH must be arm32, arm64 or x64, not {ARCH!r}')
ARM64 = ARCH == 'arm64'
X64 = ARCH == 'x64'
TRIPLE = {'arm32': 'arm-none-eabi', 'arm64': 'aarch64-none-elf', 'x64': 'x86_64-elf'}[ARCH]
SUFFIX = '' if ARCH == 'arm32' else '-'+ARCH
# ARMv7 code that QEMU's Cortex-A15 runs, scheduled for the phone's in-order
# Cortex-A53, as Thumb-2 like WebKit's own: in ARM code the engine's
# libraries took 4 MB more memory (compressed, about the same) for no speed;
# threads may start in either (arch/arm/include/arch/context.h). sdk/sos2-arm32.cmake repeats this for
# CMake builds. arm64 stays
# at ARMv8.0 (JavaScriptCore takes its CPU features from these flags) with the
# Cortex-A53 errata fixes; sdk/sos2-arm64.cmake repeats them. x64 is
# x86-64-v2 (SSE4.2, POPCNT) and RIP-relative (-fpie): programs are linked at
# 0x1010000000, beyond the small code model's absolute addresses, and the
# kernel saves no AVX state; sdk/sos2-x64.cmake repeats them.
CPU = {'arm32': ['-mthumb', '-mcpu=cortex-a15', '-mtune=cortex-a53', '-mfpu=neon-vfpv4', '-mfloat-abi=hard'],
       'arm64': ['-mcpu=cortex-a53', '-mfix-cortex-a53-835769', '-mfix-cortex-a53-843419'],
       'x64': ['-march=x86-64-v2', '-mtune=generic', '-fpie']}[ARCH]
# x64's --no-relax: ld would otherwise turn a GOT load (mov foo@GOTPCREL)
# into a 32-bit absolute immediate, which addresses at 0x1010000000 overflow;
# with it, ld makes them RIP-relative (lea foo(%rip)).
LINK = {'arm32': [], 'arm64': ['--fix-cortex-a53-843419', '--wrap=_Unwind_Find_FDE'],
        'x64': ['--wrap=_Unwind_Find_FDE', '--no-relax']}[ARCH]
CMAKE_TOOLCHAIN = ROOT/'sdk'/f'sos2-{ARCH}.cmake'
LINKER_SCRIPT = ROOT/'sdk/runtime'/{'arm32': 'linker.ld', 'arm64': 'linker-arm64.ld',
                                     'x64': 'linker-x64.ld'}[ARCH]
# Meson's host_machine for the cross builds.
MESON_CPU = {'arm32': ('arm', 'cortex-a15'), 'arm64': ('aarch64', 'cortex-a53'), 'x64': ('x86_64', 'x86_64')}[ARCH]
# ARM EABI defaults to short enums; the 64-bit ABIs have none.
SHORT_ENUMS = ARCH == 'arm32'


# A toolchain in .tools/<triple>/bin goes on PATH, so that configure scripts
# which look the target tools up by name (Newlib's, libstdc++'s) find it too.
_LOCAL_BIN = ROOT/'.tools'/TRIPLE/'bin'
if not shutil.which(TRIPLE+'-gcc') and (_LOCAL_BIN/(TRIPLE+'-gcc')).is_file():
    os.environ['PATH'] = os.pathsep.join([str(_LOCAL_BIN), os.environ.get('PATH', '')])


def prefix():
    """Use <ARCH>_PREFIX or the triple. Resolve paths from the project root
    before configure and build steps change working directory."""
    configured = os.environ.get(ARCH.upper()+'_PREFIX') or TRIPLE+'-'
    return os.path.join(ROOT, configured) if os.path.dirname(configured) else configured


def tool(name):
    """A cross tool by its short name, such as gcc, g++, ar, ranlib or strip."""
    return prefix()+name
