"""Pinned sources for the native builds: verified downloads, stamped
extraction, patches applied once and logged build steps.

A tree records its archive's SHA-256 in .sos2-source and each applied patch's
SHA-256 in its own marker file, so later runs refuse a tree from other sources.
"""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def copy_if_changed(source, destination):
    if not destination.is_file() or source.read_bytes() != destination.read_bytes():
        shutil.copyfile(source, destination)


def download(url, path, sha256):
    """Fetch a pinned archive unless a verified copy exists. The old file stays
    until the new download has been verified."""
    if path.is_file() and digest(path) == sha256:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name+'.partial')
    try:
        with urlopen(url, timeout=45) as source, temporary.open('wb') as output:
            shutil.copyfileobj(source, output)
        if digest(temporary) != sha256:
            raise RuntimeError('Source checksum mismatch: '+str(path))
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def extract_source(archive, folder, sha256):
    # Extract beside the destination and rename the stamped tree into place, so
    # an interrupted extraction never leaves a partial tree that later runs reject.
    folder.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='.'+folder.name+'-', dir=folder.parent))
    try:
        with tarfile.open(archive) as source:
            source.extractall(staging, filter='data')
        tree = staging/folder.name
        if not tree.is_dir():
            raise RuntimeError('Unexpected source archive layout: '+str(archive))
        (tree/'.sos2-source').write_text(sha256+'\n')
        tree.rename(folder)
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def source_tree(archive, folder, sha256):
    """The source tree extracted from the pinned archive, extracting it once."""
    if folder.exists():
        stamp = folder/'.sos2-source'
        if not stamp.exists() or stamp.read_text().strip() != sha256:
            raise RuntimeError('Unrecognized source tree: '+str(folder)+'; remove it to extract the pinned source again')
    elif not archive.is_file() or digest(archive) != sha256:
        raise RuntimeError('Missing/mismatched source: '+str(archive)+'; run make fetch')
    else:
        extract_source(archive, folder, sha256)
    return folder


def apply_patch(tree, patch, marker, label):
    """Apply a patch once, recording its digest in tree/marker."""
    applied, expected = tree/marker, digest(patch)
    if applied.exists():
        if applied.read_text().strip() != expected:
            raise RuntimeError(label+' patch changed; remove '+str(tree)+' to apply it to a fresh tree')
        return
    # Check the whole patch first: a hunk failing midway would otherwise leave
    # a partly applied tree that every later run rejects.
    command = ['patch', '--batch', '--forward', '--fuzz=0', '-p1', '-i', str(patch)]
    for extra in (['--dry-run'], []):
        result = subprocess.run(command+extra, cwd=tree, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            raise RuntimeError(label+' patch does not apply:\n'+result.stdout[-2000:])
    applied.write_text(expected+'\n')


def run_logged(command, log, title, cwd=None, env=None):
    """Run one build step with its output in log; a failure reports the log's tail."""
    log.parent.mkdir(parents=True, exist_ok=True)
    print(title+' build log:', log, flush=True)
    with log.open('w') as output:
        result = subprocess.run([str(x) for x in command], cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(log.read_text(errors='replace')[-4000:]+'\nSee '+str(log))
