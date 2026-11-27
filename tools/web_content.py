#!/usr/bin/env python3
"""Install the browser's local website on an existing native disk.

Every page shares the browser profile (/profiles/browser); its local website,
https://www.sos2.local/, is /data/www. Existing files are preserved unless
--replace is given. With --stick, the disk is a PC's stick image
(tools/package_pc.py) whose data volume SOS2 has formatted at its first
start: the volume in its sos2-data partition after the app store
(include/system_store.h).
"""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

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


# include/system_store.h: the data volume after the app store, at most the
# largest volume SOS2 mounts, whole 4 KiB blocks.
SECTOR,DATA_START,DATA_MAX=512,2048+122880,2097152


def stick_volume(stick):
    """The byte offset and size of a stick's data volume: its sos2-data
    partition from the primary GPT."""
    with stick.open('rb') as f:
        f.seek(SECTOR);header=f.read(92)
        if header[:8]!=b'EFI PART':raise ValueError(f'{stick} has no GPT')
        entries,count,size=struct.unpack_from('<QII',header,72)
        f.seek(entries*SECTOR);table=f.read(count*size)
    for i in range(count):
        entry=table[i*size:(i+1)*size]
        first,last=struct.unpack_from('<QQ',entry,32)
        if entry[56:128].decode('utf-16-le').rstrip('\0')=='sos2-data' and last-first+1>DATA_START:
            sectors=min(last-first+1-DATA_START,DATA_MAX)&~7
            return (first+DATA_START)*SECTOR,sectors*SECTOR
    raise ValueError(f'{stick} has no sos2-data partition')


def install_stick(stick,files,replace=False):
    """install_files on a copy of the stick's data volume, written back."""
    offset,size=stick_volume(stick)
    with tempfile.TemporaryDirectory() as directory:
        volume=Path(directory)/'volume.img'
        with stick.open('rb') as f,volume.open('wb') as out:
            f.seek(offset);out.write(f.read(size))
        install_files(volume,files,replace)
        with stick.open('r+b') as f,volume.open('rb') as data:
            f.seek(offset);shutil.copyfileobj(data,f)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disk',type=Path)
    parser.add_argument('--site',type=Path,required=True,help='folder with index.html: the local website, https://www.sos2.local/')
    parser.add_argument('--replace',action='store_true',help='explicitly replace supplied files; unrelated files are preserved')
    parser.add_argument('--stick',action='store_true',help='the disk is a PC stick image whose data volume SOS2 formatted')
    args=parser.parse_args()
    try:
        (install_stick if args.stick else install_files)(args.disk,site_files(args.site),args.replace)
    except (OSError,ValueError,RuntimeError,subprocess.CalledProcessError) as error:parser.exit(1,str(error)+'\n')
    print('Web content installed under /data/www; '+('selected files updated' if args.replace else 'existing files preserved'))


if __name__=='__main__':main()
