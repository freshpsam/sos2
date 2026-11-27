#!/usr/bin/env python3
"""Publish each device's installed system, its update and its apps on GitHub Pages.

make phone writes build/redmi2-install and build/apps (ARM32), make tablet
build/gta7lite-install and build/apps-arm64 (AArch64), make nexus5
build/nexus5-install and build/apps (the Redmi 2's ARM32 apps); this pushes
those of the devices built to the gh-pages branch of origin as a single
commit, so the branch never keeps old images: each published device's
folders and index.html are replaced, and every other file already on the
branch, another device's included, is kept. The updater (programs/updater)
downloads https://<owner>.github.io/<repository>/<device>/sos2-<device>.update
and the packages its architecture's apps folder lists. Pages is public, so a
build with a bootstrap Wi-Fi profile is refused; and the updater downloads
over Wi-Fi, so a build without the Wi-Fi firmware is refused too: a device
that installed it could not update again. The commit is built in a
temporary index; the checkout's own index and files are untouched. --device
limits it to the devices named (redmi2, gta7lite, nexus5).
"""
import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BRANCH = 'gh-pages'
APP = ROOT/'programs/updater/updater.c'
# Each device: its folder on Pages and name, its build's install and apps
# folders, and its architecture's apps folder on Pages. PCs (x64) update
# over Ethernet, without Wi-Fi firmware.
DEVICES = (('redmi2', 'Redmi 2', 'build/redmi2-install', 'build/apps', 'apps'),
           ('gta7lite', 'Galaxy Tab A7 Lite', 'build/gta7lite-install', 'build/apps-arm64', 'apps-arm64'),
           ('nexus5', 'Nexus 5', 'build/nexus5-install', 'build/apps', 'apps'),
           ('x64', 'PC', 'build/pc-install', 'build/apps-x64', 'apps-x64'))
WIRED = {'x64'}


def git(*args, env=None, data=None):
    # No optional locks: nothing here may rewrite the checkout's index.
    result = subprocess.run(['git', '--no-optional-locks', *args], cwd=ROOT, env=env, input=data,
                            check=True, capture_output=True)
    return result.stdout.decode().strip()


def pages_url():
    """https://<owner>.github.io/<repository>/ for an origin on GitHub."""
    remote = git('remote', 'get-url', 'origin')
    match = re.fullmatch(r'(?:git@[^:/]+:|ssh://git@[^/]+/|https://github\.com/)'
                         r'([A-Za-z0-9-]+)/([A-Za-z0-9._-]+?)(?:\.git)?/?', remote)
    if not match:
        raise RuntimeError('origin is not a GitHub repository: ' + remote)
    owner, repository = match.groups()
    return f'https://{owner.lower()}.github.io/{repository}/'


def load(directory, wired=False):
    """install.json and the files it lists, checked against their sizes and SHA-256s."""
    manifest = json.loads((directory/'install.json').read_text())
    if manifest.get('wifi_profile') is not False:
        raise RuntimeError(f'{directory} carries a Wi-Fi profile, and Pages is public; '
                           'build it without REDMI2_WIFI_PROFILE, GTA7LITE_WIFI_PROFILE or NEXUS5_WIFI_PROFILE')
    if not wired and manifest.get('wifi_firmware') is not True:
        raise RuntimeError(f'{directory}/install.json does not list the Wi-Fi firmware, and a device without it '
                           'could not update again; build it again with REDMI2_WIFI_FIRMWARE, GTA7LITE_STOCK '
                           'or NEXUS5_STOCK')
    files = {}
    for name, entry in manifest['files'].items():
        content = (directory/name).read_bytes()
        if len(content) != entry['bytes'] or hashlib.sha256(content).hexdigest() != entry['sha256']:
            raise RuntimeError(f'{directory/name} does not match install.json; build it again')
        files[name] = content
    files['install.json'] = (directory/'install.json').read_bytes()
    return manifest, files


def load_apps(directory):
    """index.txt and the packages it lists, checked against their sizes and SHA-256s."""
    index = (directory/'index.txt').read_bytes()
    return parse_apps(index, lambda name: (directory/name).read_bytes(), directory)


def parse_apps(index, read, where):
    """The apps an index.txt lists, read checked against their sizes and SHA-256s."""
    lines = index.decode('ascii').splitlines()
    if not lines or lines[0] != 'SOS2 apps 1':
        raise RuntimeError(f'{where}/index.txt is not an SOS2 app index; build it again')
    apps, files = [], {'index.txt': index}
    for line in lines[1:]:
        app_id, abi, size, digest, name = line.split(' ', 4)
        content = read(f'{app_id}.sos2app')
        if len(content) != int(size) or hashlib.sha256(content).hexdigest() != digest:
            raise RuntimeError(f'{where}/{app_id}.sos2app does not match index.txt; build it again')
        apps.append((app_id, name, int(size), digest))
        files[f'{app_id}.sos2app'] = content
    return apps, files


def device_section(device, title, apps_folder, manifest, apps):
    """A device's part of index.html: its release, files, first-install
    commands and its architecture's apps."""
    rows = ''.join(f'<tr><td><a href="{device}/{html.escape(name)}">{html.escape(name)}</a></td>'
                   f'<td>{entry["bytes"]:,}</td><td><code>{entry["sha256"]}</code></td></tr>\n'
                   for name, entry in manifest['files'].items())
    commands = '\n'.join(html.escape(command) for command in manifest['flash'])
    first = (manifest['first_install'] if 'first_install' in manifest else
             'a first install flashes its boot image from fastboot, after checking its SHA-256:')
    app_rows = ''.join(f'<tr><td><a href="{apps_folder}/{html.escape(app_id)}.sos2app">{html.escape(name)}</a></td>'
                       f'<td>{size:,}</td><td><code>{digest}</code></td></tr>\n'
                       for app_id, name, size, digest in apps)
    update = f'{device}/sos2-{device}.update'
    return f'''<h1>SOS2 for the {title}</h1>
<p>Release {html.escape(manifest['release'])}. Its updater installs
<a href="{update}">sos2-{device}.update</a>; {html.escape(first)}</p>
<pre>{commands}</pre>
<table>
<tr><th>File</th><th>Bytes</th><th>SHA-256</th></tr>
{rows}</table>
<p><a href="{device}/install.json">install.json</a> lists the same.</p>
<h2>Apps</h2>
<p>The updater installs these one by one, as <a href="{apps_folder}/index.txt">{apps_folder}/index.txt</a> lists them.</p>
<table>
<tr><th>App</th><th>Bytes</th><th>SHA-256</th></tr>
{app_rows}</table>
'''


def index_page(sections):
    return f'''<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SOS2</title>
{''.join(sections)}</html>
'''


def check_updater(url, device, apps_folder, board):
    """The updater must download from where this publishes: in its table of
    devices, this board's update; the apps in an APPS_URL."""
    source = APP.read_text()
    update = f'{url}{device}/sos2-{device}.update'
    if not re.search(rf'\{{\s*"{re.escape(board)}",\s*"{re.escape(update)}"', source):
        raise RuntimeError(f'the updater ({APP.relative_to(ROOT)}) must list {update} for board {board}')
    apps = f'{url}{apps_folder}/'
    if apps not in re.findall(r'#define APPS_URL\s+"([^"]+)"', source):
        raise RuntimeError(f'the updater ({APP.relative_to(ROOT)}) must have APPS_URL {apps}')


def published(ref, path):
    """A file on the branch as it is, or None."""
    try:
        return subprocess.run(['git', '--no-optional-locks', 'show', f'{ref}:{path}'], cwd=ROOT,
                              check=True, capture_output=True).stdout
    except subprocess.CalledProcessError:
        return None


def publish(only=()):
    url = pages_url()
    unknown = set(only) - {device for device, *_ in DEVICES}
    if unknown:
        raise RuntimeError(f'no such device: {", ".join(sorted(unknown))}')
    built = [(device, title, ROOT/install, ROOT/apps, folder) for device, title, install, apps, folder in DEVICES
             if (not only or device in only) and
             (ROOT/install/'install.json').is_file() and (ROOT/apps/'index.txt').is_file()]
    if not built:
        raise RuntimeError('nothing to publish: run make phone, make tablet, make nexus5 or make pc first')
    entries, releases, current = {}, {}, {}
    for device, title, install, apps_directory, folder in built:
        manifest, files = load(install, device in WIRED)
        check_updater(url, device, folder, manifest['board'])
        apps, app_files = load_apps(apps_directory)
        entries.update({f'{device}/{name}': content for name, content in files.items()})
        entries.update({f'{folder}/{name}': content for name, content in app_files.items()})
        releases[title] = manifest['release']
        current[device] = (manifest, apps)
    remote = git('ls-remote', '--heads', 'origin', BRANCH)
    ref = f'refs/remotes/origin/{BRANCH}'
    if remote:
        git('fetch', '--quiet', 'origin', f'+refs/heads/{BRANCH}:{ref}')
    # An apps folder published now replaces what every device sharing it lists.
    new_apps = {folder: current[device][1] for device, _, _, _, folder in built}
    sections = []
    for device, title, _, _, folder in DEVICES:
        if device in current:
            manifest, apps = current[device]
        elif remote and published(ref, f'{device}/install.json') is not None:
            # Another device's release stays as published.
            manifest = json.loads(published(ref, f'{device}/install.json'))
            if folder in new_apps:
                apps = new_apps[folder]
            else:
                apps, _ = parse_apps(published(ref, f'{folder}/index.txt') or b'',
                                  lambda name: published(ref, f'{folder}/{name}') or b'', folder)
        else:
            continue
        sections.append(device_section(device, title, folder, manifest, apps))
    entries['index.html'] = index_page(sections).encode()
    entries['.nojekyll'] = b''  # Serve the files as they are, without Jekyll.
    summary = ' and the '.join(f'{title} {release}' for title, release in releases.items())
    with tempfile.TemporaryDirectory(prefix='sos2-pages-') as temp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(temp)/'index'))
        if remote:
            git('read-tree', ref, env=env)
            folders = [name for device, _, _, _, folder in built for name in (device, folder)]
            git('rm', '--cached', '-r', '--quiet', '--ignore-unmatch', '--', *folders, 'index.html', env=env)
        else:
            git('read-tree', '--empty', env=env)
        for path, content in entries.items():
            blob = git('hash-object', '-w', '--stdin', data=content)
            git('update-index', '--add', '--cacheinfo', f'100644,{blob},{path}', env=env)
        tree = git('write-tree', env=env)
        commit = git('commit-tree', tree, '-m', f'Publish SOS2 for the {summary}')
    subprocess.run(['git', 'push', '--force', 'origin', f'{commit}:refs/heads/{BRANCH}'], cwd=ROOT,
                   check=True)
    print(f'Published SOS2 for the {summary} to the {BRANCH} branch:')
    print(f'  {url}')
    for device, _, _, _, folder in built:
        print(f'  {url}{device}/sos2-{device}.update and {url}{folder}/index.txt')
    print('GitHub Pages serves it within minutes when it publishes that branch '
          '(Settings > Pages: deploy from a branch, gh-pages, root).')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', action='append', default=[],
                        help='publish only this device (redmi2, gta7lite, nexus5, x64)')
    args = parser.parse_args()
    try:
        publish(args.device)
    except (OSError, KeyError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        detail = getattr(error, 'stderr', None)
        if detail:
            error = detail.decode(errors='replace').strip() or error
        sys.exit(f'Cannot publish: {error}')


if __name__ == '__main__':
    main()
