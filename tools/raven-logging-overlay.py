#!/usr/bin/env python3
"""Create a metadata-only logd overlay. Does not mount or modify any image."""
import pathlib
import sys
import tarfile

def create(output):
    # From pixelios-userland/tools/mkiosroot.sh lines 142-150.
    dirs = {'private/var/db/diagnostics': 0o751,
            'private/var/db/uuidtext': 0o755,
            'private/var/db/diagnostics/timesync': 0o755}
    with open(output, 'xb') as stream, tarfile.open(fileobj=stream, mode='w') as tar:
        for name, mode in dirs.items():
            entry = tarfile.TarInfo(name)
            entry.type = tarfile.DIRTYPE
            entry.uid = entry.gid = 272
            entry.mode = mode
            tar.addfile(entry)
            marker = tarfile.TarInfo(name + '/.migrated')
            marker.uid = marker.gid = 272
            marker.mode = 0o644
            tar.addfile(marker)
    with tarfile.open(output) as tar:
        entries = tar.getmembers()
        assert len(entries) == 6
        for entry in entries:
            assert entry.uid == entry.gid == 272
            assert not entry.issym() and not entry.islnk()
            assert entry.mode == (dirs[entry.name] if entry.isdir() else 0o644)
            assert entry.size == 0

if __name__ == '__main__':
    create(pathlib.Path(sys.argv[1]))
    print('PASS: six logd entries with numeric ownership and verified modes')
