#!/usr/bin/env python3
"""wakes-sp1 -- the built-in scale bank, from config/scales.cnfg (#44).

    gen_scales.py <scales.cnfg> <out.h>      build step (CMake runs this)

config/scales.cnfg is the DEFAULT bank: the scales Wakes uses when the drive has no
WAKES/CNFG/scales.cnfg, or when the drive's copy has a problem (all or nothing, like
engines.cnfg). End users edit the copy on the drive, which the firmware writes from this same
file when it is missing -- so out.h carries the file's text too (SP1_SCALES_TEMPLATE).

The format (sp1_cnfg.h has the device's reader, which reads it the same way):

    [3]                     a slot, 1-7
    plaits = 32             Plaits plays the degrees of weight >= 32 (default 0: all)
    tune_by_ear = off       Plaits' root stays C (default on: Tune By Ear, #30)
    ! comment               Scala's comment lines; ; and # are comments too
    name                    the Scala description
    12                      pitches that follow, the period included (1-64)
    100.0  16               cents (has a ".") or a ratio (3/2, 2), then a weight 0-255
    ...
    2/1                     the last pitch is the period

The build fails, naming the line, on anything the device would refuse -- and, unlike the
device, on a slot above [7] too: a default is never "overloaded".

Standard library only. Read as UTF-8 (with or without a BOM).
"""
import math
import os
import re
import struct
import sys

SLOTS = 7                 # SP1_SCALE_SLOTS
DEGREES = 64              # SP1_SCALE_DEGREES = marbles::kMaxDegrees (raised from 16, #44)
NAME_MAX = 24             # SP1_SCALE_NAME_MAX, the NUL included
LINE_MAX = 160            # SP1_CNFG_LINE_MAX: the device ignores longer lines


def _fail(path, line, msg):
    where = '%s:%d' % (path, line) if line else path
    sys.stderr.write('\n%s: error: %s\n\n'
                     'The built-in scales are checked at build time. See the comments\n'
                     'in config/scales.cnfg.\n' % (where, msg))
    sys.exit(1)


fail = _fail


def f32(x):
    """x rounded to a C float."""
    return struct.unpack('<f', struct.pack('<f', x))[0]


def pitch(tok):
    """A Scala pitch in volts (1 V/oct), or None."""
    if '.' in tok:
        if not re.fullmatch(r'[+-]?(\d+\.\d*|\.\d+)', tok):
            return None
        return float(tok) / 1200.0
    m = re.fullmatch(r'(\d+)(?:/(\d+))?', tok)
    if not m:
        return None
    a, b = int(m.group(1)), int(m.group(2) or 1)
    if a == 0 or b == 0 or a > 0x7FFFFFFF or b > 0x7FFFFFFF:
        return None
    return math.log2(a / b)


def cut_comment(s):
    """; or # at the start or after whitespace (sp1_cnfg.c: cut_comment)."""
    for i, c in enumerate(s):
        if c in ';#' and (i == 0 or s[i - 1].isspace()):
            return s[:i]
    return s


def name_cut(text):
    b = text.encode('utf-8')
    if len(b) <= NAME_MAX - 1:
        return b
    n = NAME_MAX - 1
    while n > 0 and (b[n] & 0xC0) == 0x80:
        n -= 1
    return b[:n]


WIKI = re.compile(r'(?i)\[\s*scales_wiki\s*\]')
COPY_FROM = re.compile(r';\s*v{4,}')         # "; vvvv ... copy from the line below"
COPY_TO = re.compile(r';\s*\^{4,}')          # "; ^^^^ ... copy down to the line above"


def parse(path):
    """The bank above [scales_wiki], and a check of every example below it."""
    with open(path, encoding='utf-8-sig') as f:
        text = f.read()
    if not text.lstrip().startswith(';'):
        fail(path, 1, 'the file must start with a comment naming its format '
                      '("; Format: scl blocks ...")')
    lines = list(enumerate(text.splitlines(), 1))
    wiki = next((i for i, (_, raw) in enumerate(lines) if WIKI.fullmatch(raw.strip())), None)
    main = lines if wiki is None else lines[:wiki]
    bank = parse_bank(main, path)
    if not any(bank):
        fail(path, 0, 'the default bank has no scale')
    if wiki is not None:
        check_examples(lines[wiki + 1:], path)
    return bank, text


def check_examples(lines, path):
    """Every block between a "; vvvv" and a "; ^^^^" line below [scales_wiki] must load as a
    scale when pasted under a slot -- the file tells users to do exactly that."""
    start = None
    for i, (ln, raw) in enumerate(lines):
        s = raw.strip()
        if COPY_FROM.match(s):
            if start is not None:
                fail(path, ln, 'a "copy from" marker before the previous block ended')
            start = i + 1
        elif COPY_TO.match(s):
            if start is None:
                fail(path, ln, 'a "copy to" marker without a "copy from" above it')
            block = lines[start:i]
            first = block[0][0] if block else ln
            bank = parse_bank([(first, '[1]')] + block, path,
                              'scale library example at line %d: ' % first)
            if bank[0] is None:
                fail(path, first, 'the scale library example at line %d is empty' % first)
            start = None
    if start is not None:
        fail(path, lines[start - 1][0], 'a "copy from" marker with no "copy to" below it')


def parse_bank(lines, path, label=''):
    """(line number, text) pairs -> the seven slots. `label` prefixes every error."""
    def fail(path, line, msg):
        _fail(path, line, label + msg)
    bank = [None] * SLOTS
    slot = None              # the section's slot; None before the first
    state = None             # 'desc', 'count', 'pitch', 'done'
    scale = None
    bang = False
    for ln, raw in lines:
        if len(raw.encode('utf-8')) > LINE_MAX:
            fail(path, ln, 'line is over %d bytes: the device would ignore it' % LINE_MAX)
        s = raw.strip()
        if s.startswith(';') or s.startswith('#'):
            continue
        if s.startswith('['):
            if state in ('count', 'pitch'):
                fail(path, ln, 'the scale in [%d] ends before its last pitch' % (slot + 1))
            m = re.fullmatch(r'\[\s*(\d+)\s*\]', s)
            if not m or not 1 <= int(m.group(1)) <= SLOTS:
                fail(path, ln, '%s is not a scale slot ([1] to [%d])' % (s, SLOTS))
            slot = int(m.group(1)) - 1
            if bank[slot] is not None:
                fail(path, ln, '[%d] is there twice' % (slot + 1))
            scale = {'name': b'', 'plaits': 0, 'plaits_set': False, 'by_ear': 1,
                     'by_ear_set': False, 'count': 0, 'p': [], 'head': ln, 'has': False}
            bank[slot] = scale
            state = 'desc'
            bang = False
            continue
        s = cut_comment(s).strip()
        if s.startswith('!'):
            bang = bang or state == 'desc'
            continue
        if slot is None:
            if s:
                fail(path, ln, '"%s" is not under a scale slot ([1] to [%d])' % (s, SLOTS))
            continue
        m = re.fullmatch(r'(?i)plaits\s*=\s*(.*)', s)
        if m and state in ('desc', 'done'):
            if scale['plaits_set']:
                fail(path, ln, 'plaits is set twice for [%d]' % (slot + 1))
            v = m.group(1).strip()
            if not re.fullmatch(r'\d+', v) or int(v) > 255:
                fail(path, ln, 'plaits must be a weight, 0-255, not "%s"' % v)
            scale['plaits'] = int(v)
            scale['plaits_set'] = True
            continue
        m = re.fullmatch(r'(?i)tune_by_ear\s*=\s*(.*)', s)
        if m and state in ('desc', 'done'):
            if scale['by_ear_set']:
                fail(path, ln, 'tune_by_ear is set twice for [%d]' % (slot + 1))
            v = m.group(1).strip().lower()
            if v not in ('on', 'off'):
                fail(path, ln, 'tune_by_ear must be on or off, not "%s"' % v)
            scale['by_ear'] = 1 if v == 'on' else 0
            scale['by_ear_set'] = True
            continue
        if state == 'desc':
            if not s and not bang:
                continue
            scale['name'] = name_cut(s)
            scale['has'] = True
            state = 'count'
        elif state == 'count':
            if not s:
                continue
            if not re.fullmatch(r'\d+', s):
                fail(path, ln, '"%s" should be the number of pitches in the scale' % s)
            c = int(s)
            if not 1 <= c <= DEGREES:
                fail(path, ln, '%d pitches: Wakes takes 1-%d, the period included' % (c, DEGREES))
            scale['count'] = c
            state = 'pitch'
        elif state == 'pitch':
            if not s:
                continue
            tok, rest = re.match(r'(\S+)\s*(.*)', s).groups()
            v = pitch(tok)
            if v is None:
                fail(path, ln, '"%s" is not a pitch: cents with a "." or a ratio' % tok)
            w = 255
            if re.fullmatch(r'\d+', rest):
                w = int(rest)
                if w > 255:
                    fail(path, ln, 'weight %d is above 255' % w)
            scale['p'].append((v, w, ln))
            if len(scale['p']) == scale['count']:
                state = 'done'
                finish(path, slot, scale, fail)
        else:                                   # done
            if s:
                fail(path, ln, '"%s" comes after the last pitch of [%d]' % (s, slot + 1))
    if state in ('count', 'pitch'):
        fail(path, 0, 'the scale in [%d] ends before its last pitch' % (slot + 1))
    for i, sc in enumerate(bank):
        if sc is not None and not sc['has']:
            bank[i] = None                      # [n] with nothing in it: an empty slot
    return bank


def finish(path, slot, sc, fail):
    period, _, pl = sc['p'][-1]
    if not period > 0:
        fail(path, pl, 'the period of [%d] must be above the root' % (slot + 1))
    degs = sorted(sc['p'][:-1], key=lambda t: t[0])
    for i, (v, w, ln) in enumerate(degs):
        if not v > 0:
            fail(path, ln, 'a pitch of [%d] is at or below the root' % (slot + 1))
        if not v < period:
            fail(path, ln, 'a pitch of [%d] is at or above the period' % (slot + 1))
        if i and v == degs[i - 1][0]:
            fail(path, ln, '[%d] has the same pitch twice' % (slot + 1))
    sc['period'] = period
    sc['degs'] = [(0.0, 255)] + [(v, w) for v, w, _ in degs]


def c_float(x):
    r = '%.9g' % f32(x)
    if 'e' not in r and '.' not in r:
        r += '.0'
    return r + 'f'


def c_bytes(b):
    out = ''
    for c in b:
        ch = chr(c)
        if ch in '\\"':
            out += '\\' + ch
        elif 0x20 <= c < 0x7f:
            out += ch
        else:
            out += '\\%03o' % c
    return '"%s"' % out


def c_string(text):
    """The text as C string literals, one per line (gen_logs.py)."""
    out = []
    for ln in text.splitlines(keepends=True):
        s = ''
        for b in ln.encode('utf-8'):
            c = chr(b)
            if c == '\\':
                s += '\\\\'
            elif c == '"':
                s += '\\"'
            elif c == '\n':
                s += '\\n'
            elif c == '\t':
                s += '\\t'
            elif 0x20 <= b < 0x7f:
                s += c
            else:
                s += '\\%03o' % b
        out.append('\t"%s"' % s)
    return '\n'.join(out) if out else '\t""'


def header(bank, text):
    L = ['/* GENERATED by tools/gen_scales.py from config/scales.cnfg -- do not edit. */',
         '#pragma once',
         '',
         '#define SP1_SCALES_GEN_SLOTS   %d' % SLOTS,
         '#define SP1_SCALES_GEN_DEGREES %d' % DEGREES,
         '',
         '/* The built-in bank (struct sp1_scale_bank, sp1_scales.h). */',
         '#define SP1_SCALES_DEFAULTS { { \\']
    for i, sc in enumerate(bank):
        if sc is None:
            L.append('\t{ 0.0f, 0, 0, 0, { 0 }, { 0.0f }, "" },  /* [%d] empty */ \\' % (i + 1))
            continue
        ws = ', '.join(str(w) for _, w in sc['degs'])
        vs = ', '.join(c_float(v) for v, _ in sc['degs'])
        L.append('\t{ %s, %d, %d, %d, \\' % (c_float(sc['period']), len(sc['degs']),
                                            sc['plaits'], sc['by_ear']))
        L.append('\t  { %s }, \\' % ws)
        L.append('\t  { %s }, \\' % vs)
        L.append('\t  %s },  /* [%d] */ \\' % (c_bytes(sc['name']), i + 1))
    L += ['} }',
          '',
          '/* config/scales.cnfg as the firmware writes it to WAKES/CNFG/ when it is missing;',
          ' * sp1_store.c defines SP1_CNFG_TEMPLATES before including this. */',
          '#if defined(SP1_CNFG_TEMPLATES)',
          'static const char SP1_SCALES_TEMPLATE[] =',
          c_string(text) + ';',
          '#endif',
          '']
    return '\n'.join(L)


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        sys.exit(2)
    src, out = sys.argv[1], sys.argv[2]
    bank, text = parse(src)
    text = header(bank, text.replace('\r\n', '\n'))
    try:
        with open(out, encoding='utf-8') as f:
            if f.read() == text:
                return          # unchanged: do not force a rebuild
    except OSError:
        pass
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


if __name__ == '__main__':
    main()
