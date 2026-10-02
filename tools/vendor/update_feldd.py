#!/usr/bin/env python3
"""wakes-sp1 -- update the vendored feldd files to a newer upstream commit.

    python tools/vendor/update_feldd.py <commit>

Deliberate, never automatic (third_party/feldd/README.md, "Updating"):

  1. clones bnjreece/feldd-sp1-firmware into a temporary directory;
  2. prints the upstream diff of the vendored files between the commit recorded in
     third_party/feldd/README.md and <commit> -- read it;
  3. copies the files at <commit> into third_party/feldd/;
  4. checks that patches/usb_midi1.patch still applies, strictly (tools/apply_patch.py).

It does NOT edit the README's recorded commit and does not commit anything: review the diff,
rebuild, run tools/host-tests, update the README, test on hardware, then commit.
Needs git on PATH. Standard library only.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
VENDOR = os.path.join(ROOT, 'third_party', 'feldd')
UPSTREAM = 'https://github.com/bnjreece/feldd-sp1-firmware.git'
# upstream path -> path under third_party/feldd
FILES = {
    'firmware/app/src/usb_midi1.c': 'src/usb_midi1.c',
    'firmware/app/src/usb_midi1.h': 'src/usb_midi1.h',
    'firmware/app/src/usb_rt_parse.c': 'src/usb_rt_parse.c',
    'firmware/app/src/usb_rt_parse.h': 'src/usb_rt_parse.h',
    'firmware/test/test_usb_rt_parse.c': 'test/test_usb_rt_parse.c',
    'LICENSE': 'LICENSE',
}


def git(args, cwd):
    return subprocess.run(['git'] + args, cwd=cwd, check=True,
                          capture_output=True, text=True).stdout


def pinned_commit():
    with open(os.path.join(VENDOR, 'README.md'), encoding='utf-8') as f:
        m = re.search(r'`([0-9a-f]{40})`', f.read())
    if not m:
        sys.exit('update_feldd: no pinned commit found in third_party/feldd/README.md')
    return m.group(1)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    new = sys.argv[1]
    old = pinned_commit()
    tmp = tempfile.mkdtemp(prefix='feldd-')
    try:
        subprocess.run(['git', 'clone', '-q', UPSTREAM, tmp], check=True)
        git(['checkout', '-q', new], tmp)
        print('=== upstream changes to the vendored files, %s..%s ===' % (old[:7], new[:7]))
        print(git(['diff', old, new, '--'] + list(FILES), tmp) or '(none)')
        for up, ours in FILES.items():
            shutil.copyfile(os.path.join(tmp, up), os.path.join(VENDOR, ours))
        out = os.path.join(tmp, 'usb_midi1.patched.c')
        r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'apply_patch.py'),
                            os.path.join(VENDOR, 'src', 'usb_midi1.c'),
                            os.path.join(VENDOR, 'patches', 'usb_midi1.patch'), out])
        if r.returncode != 0:
            print('\n!! patches/usb_midi1.patch no longer applies. Re-make it against the '
                  'new src/usb_midi1.c before building.')
            sys.exit(1)
        print('\nCopied, and the patch applies. Now: review, rebuild, run tools/host-tests, '
              'update the commit in third_party/feldd/README.md, test on hardware.')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main()
