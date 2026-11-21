"""Build the native C programs for the resource bundle: the system's updater
and the apps in apps/.

The updater, programs/updater, is part of the system like the browser: the
bundle gets it as /bin/sos2-updater, and the desktop's UPDATES button opens it
(include/apps.h). Each app is a directory apps/<id>/ holding app.json
({"name": ...}, optionally "libraries": SDK libraries from `make engine` to
link, such as mbedtls), icon.txt and its C sources. icon.txt is square pixel
art scaled to 48x48: "# " starts a comment, "color <character> <RRGGBB>"
defines a cell color, "." is transparent, and the other lines are the rows of
cells. The bundle gets /apps/<id>/program, /apps/<id>/icon and
/apps/<id>/name. Both link the SDK runtime (libsos2 and Newlib, from `make
engine`) and the desktop's drawing code, kernel/graphics.c. Only `make
engine` builds them, so the bundle carries signature(), a digest of their
sources, beside it (SOURCES_SUFFIX), which packaging checks against the
checkout (tools/project.py): a kernel whose ABI moved on without the engine
would otherwise ship an updater it refuses.
"""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import arch
import build_native_runtime as runtime

ROOT = Path(__file__).resolve().parents[1]

APPS = ROOT/'apps'
OUT = runtime.BASE/'apps'
UPDATER = ROOT/'programs/updater'
UPDATER_LIBRARIES = ['mbedtls', 'mbedx509', 'mbedcrypto']
SOURCES_SUFFIX = '.sources'
# The kernel's limits: APP_LIMIT, APP_ID_MAX, APP_NAME_MAX and APP_ICON_SIZE.
LIMITS = dict(re.findall(r'#define APP_(\w+)\s+(\d+)u?\b', (ROOT/'include/apps.h').read_text()))
APP_LIMIT, ID_MAX, NAME_MAX, ICON_SIZE = (int(LIMITS[name]) for name in ('LIMIT', 'ID_MAX', 'NAME_MAX', 'ICON_SIZE'))


def icon_pixels(path):
    """Little-endian ARGB8888 pixels from an icon.txt (see above)."""
    palette, rows = {'.': None}, []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip() or line.startswith('# '):
            continue
        if line.startswith('color '):
            parts = line.split()
            if len(parts) != 3 or len(parts[1]) != 1 or parts[1] in palette or not re.fullmatch(r'[0-9a-fA-F]{6}', parts[2]):
                raise RuntimeError(f'{path}:{number}: expected "color <character> <RRGGBB>"')
            palette[parts[1]] = int(parts[2], 16)
            continue
        rows.append((number, line))
    cells = len(rows)
    if not cells or ICON_SIZE % cells:
        raise RuntimeError(f'{path}: {cells} rows; the side must divide {ICON_SIZE}')
    for number, line in rows:
        if len(line) != cells:
            raise RuntimeError(f'{path}:{number}: rows must be {cells} cells wide')
        unknown = set(line)-set(palette)
        if unknown:
            raise RuntimeError(f'{path}:{number}: undefined colors {"".join(sorted(unknown))}')
    scale = ICON_SIZE//cells
    pixels = bytearray()
    for y in range(ICON_SIZE):
        line = rows[y//scale][1]
        for x in range(ICON_SIZE):
            color = palette[line[x//scale]]
            pixels += struct.pack('<I', 0 if color is None else 0xff000000 | color)
    return bytes(pixels)


def program(directory, out, libraries):
    """directory's C sources linked with the SDK libraries into out/program, stripped."""
    for library in libraries:
        if not re.fullmatch(r'[a-z0-9_]+', library) or not (runtime.SYSROOT/'lib'/f'lib{library}.a').is_file():
            raise RuntimeError(f'{directory}: no SDK library {library!r}; run make engine')
    sources = sorted(directory.glob('*.c'))
    if not sources:
        raise RuntimeError(f'{directory}: no C sources')
    out.mkdir(parents=True, exist_ok=True)
    linked, stripped = out/'program.elf', out/'program'
    subprocess.run([arch.tool('gcc'), *runtime.sdk_flags(), '-Wall', '-Wextra', '-Werror',
                    *map(str, sources), str(ROOT/'kernel/graphics.c'), '-o', str(linked),
                    *runtime.link_flags(), *[f'-l{library}' for library in libraries],
                    *runtime.link_libraries()], cwd=ROOT, check=True)
    subprocess.run([arch.tool('strip'), '--strip-unneeded', '-o', str(stripped), str(linked)], check=True)
    return stripped


def build(directory):
    app = directory.name
    if not re.fullmatch(f'[a-z0-9-]{{1,{ID_MAX}}}', app):
        raise RuntimeError(f'{directory}: app directories are 1-{ID_MAX} lowercase letters, digits and hyphens')
    manifest = json.loads((directory/'app.json').read_text())
    name, libraries = manifest['name'], manifest.get('libraries', [])
    if not 1 <= len(name) <= NAME_MAX or not all(' ' <= c <= '~' for c in name):
        raise RuntimeError(f'{directory}/app.json: the name must be 1-{NAME_MAX} printable ASCII characters')
    if not isinstance(libraries, list) or not all(isinstance(library, str) for library in libraries):
        raise RuntimeError(f'{directory}/app.json: "libraries" is a list of names')
    return {f'/apps/{app}/program': program(directory, OUT/app, libraries),
            f'/apps/{app}/icon': icon_pixels(directory/'icon.txt'),
            f'/apps/{app}/name': name.encode()}


def program_directories():
    return [UPDATER, *sorted(path for path in APPS.iterdir() if path.is_dir())]


def sources():
    """The files the programs come from: what the compiler reads for the
    updater and each app (its -MM list, system headers aside), the apps'
    app.json and icon.txt, and the SDK, libsos2's sources included."""
    found = set()
    for directory in program_directories():
        inputs = [*sorted(directory.glob('*.c')), ROOT/'kernel/graphics.c']
        listed = subprocess.run([arch.tool('gcc'), *runtime.sdk_flags(), '-MM', *map(str, inputs)],
                                cwd=ROOT, check=True, capture_output=True, text=True).stdout
        found.update((ROOT/word).resolve() for word in listed.replace('\\\n', ' ').split()
                     if not word.endswith(':'))
        found.update(directory/name for name in ('app.json', 'icon.txt') if (directory/name).is_file())
    found.update(path.resolve() for path in (ROOT/'sdk').rglob('*') if path.is_file())
    return sorted(found)


def signature():
    """A digest of sources(): their paths in the checkout and their bytes."""
    digest = hashlib.sha256()
    for path in sources():
        name, content = str(path.relative_to(ROOT)).encode(), path.read_bytes()
        digest.update(len(name).to_bytes(4, 'little') + name + len(content).to_bytes(8, 'little') + content)
    return digest.hexdigest()


def files():
    """Bundle entries for the updater and every app; needs the SDK from `make engine`."""
    if not (runtime.BASE/'sdk/libsos2.a').exists():
        raise RuntimeError('Build the native SDK first (make engine)')
    directories = program_directories()[1:]
    if len(directories) > APP_LIMIT:
        raise RuntimeError(f'at most {APP_LIMIT} apps fit the launcher')
    entries = {'/bin/sos2-updater': program(UPDATER, runtime.BASE/'updater', UPDATER_LIBRARIES)}
    for directory in directories:
        entries.update(build(directory))
    return entries
