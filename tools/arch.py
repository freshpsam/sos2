"""The target architecture of the native builds: SOS2_ARCH=arm32 (the default:
QEMU's ARM machine and the Redmi 2) or arm64 (QEMU's AArch64 machine and the
Galaxy Tab A7 Lite).

ARM32 keeps the unsuffixed build directories; arm64 builds into ones ending in
-arm64. A cross tool is found on PATH, through <ARCH>_PREFIX, or in
.tools/<triple>/bin (an unpacked Arm GNU Toolchain).
"""
import os
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
ARCH = os.environ.get('SOS2_ARCH', 'arm32')
if ARCH not in ('arm32', 'arm64'):
    raise SystemExit(f'SOS2_ARCH must be arm32 or arm64, not {ARCH!r}')
ARM64 = ARCH == 'arm64'
TRIPLE = 'aarch64-none-elf' if ARM64 else 'arm-none-eabi'
SUFFIX = '-arm64' if ARM64 else ''
# ARMv7 code that QEMU's Cortex-A15 runs, scheduled for the phone's in-order
# Cortex-A53; sdk/sos2-arm32.cmake repeats this for CMake builds. arm64 stays
# at ARMv8.0 (JavaScriptCore takes its CPU features from these flags) with the
# Cortex-A53 errata fixes; sdk/sos2-arm64.cmake repeats them.
CPU = (['-mcpu=cortex-a53', '-mfix-cortex-a53-835769', '-mfix-cortex-a53-843419'] if ARM64 else
       ['-marm', '-mcpu=cortex-a15', '-mtune=cortex-a53', '-mfpu=neon-vfpv4', '-mfloat-abi=hard'])
LINK = ['--fix-cortex-a53-843419', '--wrap=_Unwind_Find_FDE'] if ARM64 else []
CMAKE_TOOLCHAIN = ROOT/'sdk'/f'sos2-{ARCH}.cmake'
LINKER_SCRIPT = ROOT/'sdk/runtime'/('linker-arm64.ld' if ARM64 else 'linker.ld')
# Meson's host_machine for the cross builds.
MESON_CPU = ('aarch64', 'cortex-a53') if ARM64 else ('arm', 'cortex-a15')
# ARM EABI defaults to short enums; AArch64's ABI has none.
SHORT_ENUMS = not ARM64


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
