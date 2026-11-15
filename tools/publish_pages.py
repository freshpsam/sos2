#!/usr/bin/env python3
"""Publish the installed Redmi 2 system, its update and the apps on GitHub Pages.

make phone writes build/redmi2-install and build/apps; this pushes those files
to the gh-pages branch of origin as a single commit, so the branch never keeps
old images: redmi2/, apps/ and index.html are replaced and every other file
already on the branch is kept. The phone's updater (programs/updater)
downloads https://<owner>.github.io/<repository>/redmi2/sos2-redmi2.update and
the packages apps/index.txt lists. Pages is public, so a build with a
bootstrap Wi-Fi profile is refused. The commit is built in a temporary index;
the checkout's own index and files are untouched.
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


def load(directory):
    """install.json and the files it lists, checked against their sizes and SHA-256s."""
    manifest = json.loads((directory/'install.json').read_text())
    if manifest.get('wifi_profile') is not False:
        raise RuntimeError('this build carries a Wi-Fi profile (REDMI2_WIFI_PROFILE), and Pages is '
                           'public; run make phone without it')
    files = {}
    for name, entry in manifest['files'].items():
        content = (directory/name).read_bytes()
        if len(content) != entry['bytes'] or hashlib.sha256(content).hexdigest() != entry['sha256']:
            raise RuntimeError(f'{name} does not match install.json; run make phone again')
        files[name] = content
    files['install.json'] = (directory/'install.json').read_bytes()
    return manifest, files


def load_apps(directory):
    """index.txt and the packages it lists, checked against their sizes and SHA-256s."""
    index = (directory/'index.txt').read_bytes()
    lines = index.decode('ascii').splitlines()
    if not lines or lines[0] != 'SOS2 apps 1':
        raise RuntimeError('apps/index.txt is not an SOS2 app index; run make phone again')
    apps, files = [], {'index.txt': index}
    for line in lines[1:]:
        app_id, abi, size, digest, name = line.split(' ', 4)
        content = (directory/f'{app_id}.sos2app').read_bytes()
        if len(content) != int(size) or hashlib.sha256(content).hexdigest() != digest:
            raise RuntimeError(f'{app_id}.sos2app does not match index.txt; run make phone again')
        apps.append((app_id, name, int(size), digest))
        files[f'{app_id}.sos2app'] = content
    return apps, files


def index_page(manifest, apps):
    rows = ''.join(f'<tr><td><a href="redmi2/{html.escape(name)}">{html.escape(name)}</a></td>'
                   f'<td>{entry["bytes"]:,}</td><td><code>{entry["sha256"]}</code></td></tr>\n'
                   for name, entry in manifest['files'].items())
    commands = '\n'.join(html.escape(command) for command in manifest['flash'])
    app_rows = ''.join(f'<tr><td><a href="apps/{html.escape(app_id)}.sos2app">{html.escape(name)}</a></td>'
                       f'<td>{size:,}</td><td><code>{digest}</code></td></tr>\n'
                       for app_id, name, size, digest in apps)
    return f'''<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SOS2</title>
<h1>SOS2 for the Redmi 2</h1>
<p>Release {html.escape(manifest['release'])}. The phone's updater installs
<a href="redmi2/sos2-redmi2.update">sos2-redmi2.update</a>; a first install flashes the two images
from lk2nd's fastboot, after checking their SHA-256:</p>
<pre>{commands}</pre>
<table>
<tr><th>File</th><th>Bytes</th><th>SHA-256</th></tr>
{rows}</table>
<p><a href="redmi2/install.json">install.json</a> lists the same, with the boot command line.</p>
<h2>Apps</h2>
<p>The updater installs these one by one, as <a href="apps/index.txt">apps/index.txt</a> lists them.</p>
<table>
<tr><th>App</th><th>Bytes</th><th>SHA-256</th></tr>
{app_rows}</table>
</html>
'''


def publish(directory, apps_directory):
    url = pages_url()
    update = url + 'redmi2/sos2-redmi2.update'
    source = APP.read_text()
    for name, wanted in (('UPDATE_URL', update), ('APPS_URL', url + 'apps/')):
        match = re.search(rf'#define {name}\s+"([^"]+)"', source)
        if not match or match.group(1) != wanted:
            raise RuntimeError(f'the updater ({APP.relative_to(ROOT)}) must have {name} {wanted}')
    manifest, files = load(directory)
    apps, app_files = load_apps(apps_directory)
    with tempfile.TemporaryDirectory(prefix='sos2-pages-') as temp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(temp)/'index'))
        if git('ls-remote', '--heads', 'origin', BRANCH):
            git('fetch', '--quiet', 'origin', f'+refs/heads/{BRANCH}:refs/remotes/origin/{BRANCH}')
            git('read-tree', f'refs/remotes/origin/{BRANCH}', env=env)
            git('rm', '--cached', '-r', '--quiet', '--ignore-unmatch', '--', 'redmi2', 'apps',
                'index.html', env=env)
        else:
            git('read-tree', '--empty', env=env)
        entries = {f'redmi2/{name}': content for name, content in files.items()}
        entries.update({f'apps/{name}': content for name, content in app_files.items()})
        entries['index.html'] = index_page(manifest, apps).encode()
        entries['.nojekyll'] = b''  # Serve the files as they are, without Jekyll.
        for path, content in entries.items():
            blob = git('hash-object', '-w', '--stdin', data=content)
            git('update-index', '--add', '--cacheinfo', f'100644,{blob},{path}', env=env)
        tree = git('write-tree', env=env)
        commit = git('commit-tree', tree, '-m', f'Publish SOS2 {manifest["release"]} for the Redmi 2')
    subprocess.run(['git', 'push', '--force', 'origin', f'{commit}:refs/heads/{BRANCH}'], cwd=ROOT,
                   check=True)
    print(f'Published {manifest["release"]} to the {BRANCH} branch:')
    print(f'  {url}')
    print(f'  {update}')
    print(f'  {url}apps/index.txt ({len(apps)} apps)')
    print('GitHub Pages serves it within minutes when it publishes that branch '
          '(Settings > Pages: deploy from a branch, gh-pages, root).')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path, help='install files from make phone')
    parser.add_argument('apps', type=Path, help='app packages and index.txt from make phone')
    args = parser.parse_args()
    try:
        publish(args.directory, args.apps)
    except (OSError, KeyError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        detail = getattr(error, 'stderr', None)
        if detail:
            error = detail.decode(errors='replace').strip() or error
        sys.exit(f'Cannot publish: {error}')


if __name__ == '__main__':
    main()
