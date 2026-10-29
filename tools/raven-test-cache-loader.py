#!/usr/bin/env python3
"""Test 0207 against a complete XNU source tree already patched through 0206."""
import hashlib
import itertools
import pathlib
import re
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
source = pathlib.Path(sys.argv[1]) / 'bsd/kern/kern_exec.c'
before = source.read_text()
patch = repo / 'xnu/patches/0207-raven-launchd-cache-loader-platform.patch'
with tempfile.TemporaryDirectory() as tmp:
    root = pathlib.Path(tmp)
    target = root / 'bsd/kern/kern_exec.c'
    target.parent.mkdir(parents=True)
    target.write_text(before)
    subprocess.run(['git', 'apply', '--check', str(patch)], cwd=root, check=True)
    subprocess.run(['git', 'apply', str(patch)], cwd=root, check=True)
    after = target.read_text()
    expected = before.replace('strcmp(p->p_name, "dirs_cleaner") == 0;',
        'strcmp(p->p_name, "dirs_cleaner") == 0 ||\n'
        '\t\t    /* p_name holds 32 chars; p_comm holds only 16. Match the full name. */\n'
        '\t\t    strcmp(p->p_name, "launchd_cache_loader") == 0;')
    assert after == expected and after != before, 'unexpected changes outside name predicate'
    uncommented = re.sub(r'/\*.*?\*/', '', after, flags=re.S)
    names = re.search(r'boolean_t sweet_boot_task_name =\s*(.*?);', uncommented, re.S)[1]
    gate = re.search(r'else if \((sweet_boot_task_name && sweet_boot_task_parent &&.*?\)) \{', after, re.S)[1]
    # Compile the actual extracted name predicate and acceptance gate, not a Python copy.
    c = '#include <string.h>\n#include <stdlib.h>\n'
    c += 'int main(int argc, char **argv) { (void)argc;\n'
    c += 'struct { char p_name[33]; } proc = {0}, *p = &proc;\n'
    c += 'strncpy(p->p_name, argv[1], sizeof(p->p_name)-1);\n'
    c += 'struct {int psa_platform;} attr, *psa = &attr;\n'
    c += 'attr.psa_platform = atoi(argv[5]); if (attr.psa_platform == -1) psa = 0;\n'
    c += 'int sweet_boot_task_parent=atoi(argv[2]), sweet_boot_task_signed=atoi(argv[3]);\n'
    c += 'int sweet_boot_task_root_ro=atoi(argv[4]);\n'
    c += 'int sweet_boot_task_name = ' + names + ';\n'
    c += 'return (' + gate[:-1] + ') ? 0 : 1; }\n'
    (root/'test.c').write_text(c)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(root/'test.c'), '-o', str(root/'test')], check=True)
    count = 0
    for name, parent, signed, approved, platform in itertools.product(
        ['launchd_cache_loader', 'launchd_cache_l', 'launchd_cache_lo',
         'launchd_cache_loa', 'launchd_cache_loaderX', 'other', 'dirs_cleaner'],
        [0, 1], [0, 1], [0, 1], [-1, 0, 2]):
        want = name in ('launchd_cache_loader', 'dirs_cleaner') and parent and signed and approved and platform in (-1, 0)
        result = subprocess.run([str(root/'test'), name, str(parent), str(signed), str(approved), str(platform)])
        assert result.returncode == (0 if want else 1), (name, parent, signed, approved, platform)
        count += 1
    # Duplicate application must fail, then reverse must recover exact baseline.
    assert subprocess.run(['git', 'apply', '--check', str(patch)], cwd=root, capture_output=True).returncode != 0
    subprocess.run(['git', 'apply', '-R', str(patch)], cwd=root, check=True)
    assert target.read_text() == before
print(f'PASS: patch/reverse/duplicate checks and {count} compiled predicate cases')
print('Baseline SHA256:', hashlib.sha256(before.encode()).hexdigest())
