#!/usr/bin/env python3
"""Install the browser's local website on an existing native disk.

Every page shares the browser profile (/profiles/browser); its local website,
https://www.sos2.local/, is /data/www. Existing files are preserved unless
--replace is given.
"""
import argparse
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
TOOL=ROOT/'build/host/fs-image'
PROFILE='browser'


def install_files(disk,files,replace=False):
    if not TOOL.is_file():raise RuntimeError('Build the disk editor with make build/host/fs-image')
    # Publish the entry document last. Each individual file is atomically renamed.
    for name,source in sorted(files.items(),key=lambda item:(item[0]=='index.html',item[0])):
        path=Path(name)
        if path.is_absolute() or '..' in path.parts or '\\' in name:raise ValueError('Invalid site path: '+name)
        subprocess.run([TOOL,disk,PROFILE,'put' if replace else 'create','/www/'+name,source],check=True)


def site_files(directory):
    root=Path(directory);files={}
    if root.is_symlink():raise ValueError('Site imports must not contain symlinks: '+str(root))
    for path in sorted(root.rglob('*')):
        if path.is_symlink():raise ValueError('Site imports must not contain symlinks: '+str(path))
        if path.is_file():files[path.relative_to(root).as_posix()]=path
    if 'index.html' not in files:raise ValueError('Site requires index.html: '+str(root))
    return files


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disk',type=Path)
    parser.add_argument('--site',type=Path,required=True,help='folder with index.html: the local website, https://www.sos2.local/')
    parser.add_argument('--replace',action='store_true',help='explicitly replace supplied files; unrelated files are preserved')
    args=parser.parse_args()
    try:
        install_files(args.disk,site_files(args.site),args.replace)
    except (OSError,ValueError,RuntimeError,subprocess.CalledProcessError) as error:parser.exit(1,str(error)+'\n')
    print('Web content installed under /data/www; '+('selected files updated' if args.replace else 'existing files preserved'))


if __name__=='__main__':main()
