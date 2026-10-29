#!/usr/bin/env python3
"""Read service definitions from mounted Apple SOURCE and staged ROOT. No changes.

Requires real mount directories; rejects symlinks to avoid following iOS paths
into the host. Results are candidates for review, not permission to enable jobs.
"""
import hashlib
import json
import pathlib
import plistlib
import sys

TERMS = ('logd', 'notifyd', 'configd', 'securityd', 'runningboard', 'watchdog',
         'cfpref', 'distnoted', 'containermanager', 'mobilegestalt', 'backboard', 'springboard')

def safe(root, path):
    path = pathlib.PurePosixPath(path)
    if not path.is_absolute() or '..' in path.parts:
        raise ValueError('not an absolute root-contained path')
    result = root
    for part in path.parts[1:]:
        result = result / part
        if result.is_symlink():
            raise ValueError('symlink requires iOS-root-aware review: ' + str(path))
    return result

def review(source, staged):
    rows = []
    paths = [source/'System/Library/xpc/launchd.plist']
    for base in ('System/Library/LaunchDaemons', 'Library/LaunchDaemons'):
        directory = safe(source, '/' + base)
        paths.extend(sorted(directory.glob('*.plist')))
    for path in paths:
        path = safe(source, '/' + path.relative_to(source).as_posix())
        if not path.is_file():
            continue
        obj = plistlib.loads(path.read_bytes())
        jobs = obj.get('LaunchDaemons', {obj.get('Label', path.stem): obj})
        for key, job in jobs.items():
            if not isinstance(job, dict):
                continue
            args = job.get('ProgramArguments', [])
            executable = job.get('Program') or (args[0] if args else None)
            label = str(job.get('Label', key))
            if not any(term in (label + ' ' + str(executable)).lower() for term in TERMS):
                continue
            row = {'plist': '/' + path.relative_to(source).as_posix(), 'label': label,
                   'program': executable, 'definition': job,
                   'plist_sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
            if executable:
                for scope, root in [('source', source), ('staged', staged)]:
                    try:
                        binary = safe(root, executable)
                        row[scope + '_present'] = binary.is_file()
                        if binary.is_file():
                            row[scope + '_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
                            row[scope + '_executable_mode'] = bool(binary.stat().st_mode & 0o111)
                    except ValueError as error:
                        row[scope + '_review_required'] = str(error)
            rows.append(row)
    return rows

if __name__ == '__main__':
    source, staged = map(lambda s: pathlib.Path(s).resolve(strict=True), sys.argv[1:])
    print(json.dumps(review(source, staged), indent=2, default=str))
