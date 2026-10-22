#!/usr/bin/env python3
"""Provenance for boot images, so an old or wrong image is never booted by mistake.

  imgmeta.py write IMG --kind xnu|probe|udump [--args STR]   write IMG.meta.json (sha256, git head, source hash, time)
  imgmeta.py check IMG --expect KIND [--allow-stale] [--max-age-min N]   exit 0 only if IMG is what it claims to be

`check` fails when: the sidecar is missing, the image bytes differ from the recorded sha256, the kind is not the one
expected, the sources the image was built from have changed since (stale), or the image is too old.
"""
import argparse, hashlib, json, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = ['shim', 'soc', 'device', 'xnu/patches', 'qrlog', 'xnu/qr', 'tools/mkadt.py', 'tools/mkpayload.py', 'tools/boards.py']


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def tracked():
    try:
        return subprocess.check_output(['git', 'ls-files', *SRC], cwd=ROOT, text=True, stderr=subprocess.DEVNULL).split('\n')
    except (subprocess.CalledProcessError, OSError):  # not a git checkout (zip, generated tree): walk SRC instead
        out = []
        for s in SRC:
            if os.path.isfile(os.path.join(ROOT, s)):
                out.append(s)
            for d, _, fs in os.walk(os.path.join(ROOT, s)):
                if '__pycache__' not in d:
                    out += [os.path.relpath(os.path.join(d, f), ROOT) for f in fs]
        return out


def srchash():
    files = tracked()
    h = hashlib.sha256()
    for f in sorted(x for x in files if x and '/out/' not in x):
        p = os.path.join(ROOT, f)
        if os.path.isfile(p):
            h.update(f.encode() + b'\0')
            h.update(open(p, 'rb').read())
    return h.hexdigest()


def head():
    try:
        return subprocess.check_output(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, text=True, stderr=subprocess.DEVNULL).strip()
    except (subprocess.CalledProcessError, OSError):
        return 'nogit'  # fallback id: not a git checkout


def meta_path(img):
    return img + '.meta.json'


def write(a):
    m = {'sha256': sha(a.img), 'size': os.path.getsize(a.img), 'kind': a.kind, 'args': a.args or '',
         'git_head': head(), 'src_hash': srchash(), 'built_at': int(time.time())}
    json.dump(m, open(meta_path(a.img), 'w'), indent=1)
    print('imgmeta: %s kind=%s head=%s' % (os.path.basename(a.img), a.kind, m['git_head']))


def check(a):
    err = []
    if 'lz4' in os.path.basename(a.img):
        err.append('lz4 images are silently rejected by this bootloader (docs/HANDOFF.md)')
    try:
        m = json.load(open(meta_path(a.img)))
    except OSError:
        err.append('no %s: rebuild with the build script' % os.path.basename(meta_path(a.img)))
        m = None
    if m:
        if sha(a.img) != m['sha256']:
            err.append('image bytes changed since it was built (sha256 mismatch)')
        if m['kind'] != a.expect:
            err.append('image kind is %r, expected %r' % (m['kind'], a.expect))
        if not a.allow_stale and srchash() != m['src_hash']:
            err.append('sources changed since this image was built (built at %s): rebuild it' % m['git_head'])
        age = (time.time() - m['built_at']) / 60
        if a.max_age_min and age > a.max_age_min:
            err.append('image is %.0f min old (limit %d)' % (age, a.max_age_min))
        print('image: kind=%s built=%s head=%s age=%.0f min args=%s' % (m['kind'], time.strftime('%H:%M:%S', time.localtime(m['built_at'])), m['git_head'], age, m['args'] or '-'))
    for e in err:
        print('REFUSED: ' + e, file=sys.stderr)
    return 1 if err else 0


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    s = p.add_subparsers(dest='cmd', required=True)
    w = s.add_parser('write'); w.add_argument('img'); w.add_argument('--kind', required=True); w.add_argument('--args')
    c = s.add_parser('check'); c.add_argument('img'); c.add_argument('--expect', required=True)
    c.add_argument('--allow-stale', action='store_true'); c.add_argument('--max-age-min', type=int, default=0)
    a = p.parse_args()
    sys.exit(write(a) or 0 if a.cmd == 'write' else check(a))
