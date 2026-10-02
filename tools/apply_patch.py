#!/usr/bin/env python3
"""wakes-sp1 -- apply a one-file unified diff to a vendored file, STRICTLY.

    apply_patch.py <pristine file> <patch> <output file>

Used by the build (firmware/CMakeLists.txt) and the host tests to turn a pristine vendored
file -- third_party/feldd/src/usb_midi1.c -- into the version Wakes compiles, without ever
editing the vendored copy. The same rule as sp1_plaits_override(): if upstream has changed
under the patch, the build STOPS with the hunk that no longer matches. There is no fuzz and
no "close enough": a patch that half-applies is exactly the silent failure this exists to
prevent.

  - Every hunk's old text (context and removed lines) must be found verbatim. If it occurs
    more than once in the file, it must occur at the line the hunk names (after the offset
    of the hunks before it); anything else is an error.
  - Line endings are normalised to LF on the way in (a Windows checkout gives CRLF) and the
    output is written with LF.
  - The output is only rewritten when its content changes, so a reconfigure does not force
    a rebuild.

Standard library only.
"""
import os
import re
import sys

HUNK = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


def fail(msg):
    sys.stderr.write('apply_patch: ' + msg + '\n')
    sys.exit(1)


def read_lines(path):
    with open(path, 'rb') as f:
        text = f.read().decode('utf-8').replace('\r\n', '\n')
    return text.split('\n')


def parse(patch_lines):
    hunks = []
    cur = None
    for ln in patch_lines:
        m = HUNK.match(ln)
        if m:
            cur = {'old_start': int(m.group(1)), 'old': [], 'new': []}
            hunks.append(cur)
            continue
        if cur is None or ln.startswith('---') or ln.startswith('+++'):
            continue
        if ln.startswith(' '):
            cur['old'].append(ln[1:])
            cur['new'].append(ln[1:])
        elif ln.startswith('-'):
            cur['old'].append(ln[1:])
        elif ln.startswith('+'):
            cur['new'].append(ln[1:])
        elif ln.startswith('\\'):
            continue                      # "\ No newline at end of file"
        elif ln == '':
            continue                      # trailing newline of the patch file
        else:
            fail('unexpected line in patch: %r' % ln)
    if not hunks:
        fail('no hunks in patch')
    return hunks


def find_all(lines, block):
    n = len(block)
    return [i for i in range(len(lines) - n + 1) if lines[i:i + n] == block]


def apply(lines, hunks, patch_name):
    offset = 0
    for k, h in enumerate(hunks):
        hits = find_all(lines, h['old'])
        want = h['old_start'] - 1 + offset
        if len(hits) == 1:
            at = hits[0]
        elif want in hits:
            at = want
        else:
            fail('%s: hunk %d (@@ -%d) does not match the vendored file %s. '
                 'Upstream changed? Re-check the patch.'
                 % (patch_name, k + 1, h['old_start'],
                    'anywhere' if not hits else 'at its own line (%d candidates)' % len(hits)))
        lines[at:at + len(h['old'])] = h['new']
        offset += len(h['new']) - len(h['old'])
    return lines


def main():
    if len(sys.argv) != 4:
        fail('usage: apply_patch.py <pristine> <patch> <output>')
    src, patch, out = sys.argv[1:]
    lines = apply(read_lines(src), parse(read_lines(patch)), os.path.basename(patch))
    text = '\n'.join(lines)
    old = None
    if os.path.exists(out):
        with open(out, 'rb') as f:
            old = f.read().decode('utf-8')
    if old != text:
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        with open(out, 'wb') as f:
            f.write(text.encode('utf-8'))


if __name__ == '__main__':
    main()
