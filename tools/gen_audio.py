#!/usr/bin/env python3
"""wakes-sp1 -- generate the firmware's audio settings from config/audio.cnfg.

    gen_audio.py <audio.cnfg> <out.h>     build step (CMake runs this)

config/audio.cnfg holds the BUILT-IN DEFAULTS (#52) of the user's choices for how Wakes' sound leaves the device: today, how
USB audio out sets its level and whether the speaker plays while a host records over USB.
This file holds the facts the user cannot choose (VOL's steps).

Users edit the copy on the drive, WAKES/CNFG/audio.cnfg, read at every ON by the firmware's
own reader (firmware/src/sp1_cnfg.c: a bad line keeps its default). The build fails, naming
the line, on anything invalid in the DEFAULT -- an unknown section or key, a value that is
not one of the choices, a level that is not one of VOL's steps.

Standard library only. Read as UTF-8 (with or without a BOM).
"""
import configparser
import os
import re
import sys

# ---- facts: NOT user configuration ---------------------------------------------------------
# VOL's steps (firmware/src/sp1_audio.c, TONE_AMP): 0 dB, then 3 dB apart down to -30 dB; step
# n is -3n dB. A parked level is one of them, because parking IS setting VOL (no second gain
# stage -- that is what keeps it free of CPU).
VOL_STEP_DB = 3
VOL_STEPS = 11                       # 0 .. -30 dB (the 12th step is mute)

LEVEL = {'parked': 1, 'vol': 0}
ONOFF = {'on': 1, 'off': 0}
KEYS = {'usb': {'level', 'parked_level', 'speaker'}}
DEFAULTS = {'level': 'parked', 'parked_level': '-6', 'speaker': 'off'}

def c_template(name, text):
    """The file as a C string for the firmware to write to WAKES/CNFG/ when it is missing (#52).
    Non-ASCII as octal escapes (a hex escape would swallow the hex digits after it)."""
    out = ['#if defined(SP1_CNFG_TEMPLATES)', 'static const char %s[] =' % name]
    for ln in text.splitlines(keepends=True):
        lit = ''
        for b in ln.encode('utf-8'):
            ch = chr(b)
            if ch == '\\':
                lit += '\\\\'
            elif ch == '"':
                lit += '\\"'
            elif ch == '\n':
                lit += '\\n'
            elif ch == '\t':
                lit += '\\t'
            elif 0x20 <= b < 0x7f:
                lit += ch
            else:
                lit += '\\%03o' % b
        out.append('\t"%s"' % lit)
    out += ['\t;' if text else '\t"";', '#endif']
    return out


def check_lines(path, text, fail_fn):
    """#52: the first line names the format, and no line is longer than the device reads."""
    if not text.lstrip().startswith(';'):
        fail_fn(path, 1, 'the file must start with a comment naming its format ("; Format: ini")')
    for i, ln in enumerate(text.splitlines(), 1):
        if len(ln.encode('utf-8')) > 160:
            fail_fn(path, i, 'line is over 160 bytes: the device would ignore it')



def fail(path, line, msg):
    where = '%s:%d' % (path, line) if line else path
    sys.stderr.write('\n%s: error: %s\n\n'
                     'The audio settings are checked at build time so a mistake cannot reach\n'
                     'the device. See the comments in config/audio.cnfg.\n' % (where, msg))
    sys.exit(1)


def line_of(text, section, key):
    """Line number of `key` inside [section], for error messages."""
    cur = None
    for i, ln in enumerate(text.splitlines(), 1):
        s = ln.strip()
        m = re.match(r'^\[(.+)\]$', s)
        if m:
            cur = m.group(1).strip().lower()
        elif cur == section and re.match(r'^%s\s*[=:]' % re.escape(key), s, re.I):
            return i
    return 0


def parse(path):
    with open(path, encoding='utf-8-sig') as f:
        text = f.read()
    check_lines(path, text, fail)
    cp = configparser.ConfigParser(inline_comment_prefixes=(';', '#'),
                                   comment_prefixes=(';', '#'), strict=True,
                                   interpolation=None)
    try:
        cp.read_string(text, source=path)
    except configparser.Error as e:
        fail(path, getattr(e, 'lineno', 0), str(e).splitlines()[0])

    def err(sec, key, msg):
        fail(path, line_of(text, sec, key), msg)

    for sec in cp.sections():
        if sec not in KEYS:
            fail(path, 0, 'unknown section [%s] (expected %s)'
                 % (sec, ', '.join('[%s]' % s for s in sorted(KEYS))))
        for key in cp[sec]:
            if key not in KEYS[sec]:
                err(sec, key, 'unknown setting "%s" in [%s] (expected %s)'
                    % (key, sec, ', '.join(sorted(KEYS[sec]))))

    usb = cp['usb'] if cp.has_section('usb') else {}

    def get(key):
        return usb.get(key, DEFAULTS[key]).strip().lower()

    cfg = {}
    v = get('level')
    if v not in LEVEL:
        err('usb', 'level', 'level must be parked or vol, not "%s"' % v)
    cfg['parked'] = LEVEL[v]

    v = get('parked_level')
    m = re.fullmatch(r'([+-]?\d+)\s*(db)?', v)
    if not m:
        err('usb', 'parked_level', 'parked_level must be a number of dB, like -6, not "%s"' % v)
    db = int(m.group(1))
    if db > 0 or db % VOL_STEP_DB != 0 or -db > (VOL_STEPS - 1) * VOL_STEP_DB:
        err('usb', 'parked_level', 'parked_level must be one of VOL\'s steps: %s dB, not %d'
            % (', '.join(str(-VOL_STEP_DB * n) for n in range(VOL_STEPS)), db))
    cfg['parked_db'] = db
    cfg['parked_step'] = -db // VOL_STEP_DB

    v = get('speaker')
    if v not in ONOFF:
        err('usb', 'speaker', 'speaker must be on or off, not "%s"' % v)
    cfg['speaker'] = ONOFF[v]
    return cfg


def header(cfg, src, text):
    return '\n'.join([
        '/* GENERATED by tools/gen_audio.py from %s -- do not edit. */' % src,
        '#pragma once',
        '',
        '/* [usb] level: 1 = parked (VOL set to the parked step and locked while a host takes',
        ' * USB audio out), 0 = vol (VOL moves USB audio out with everything else). */',
        '#define SP1_AUDIO_USB_LEVEL_PARKED   %d' % cfg['parked'],
        '/* [usb] parked_level, as VOL\'s step (sp1_audio.c TONE_AMP) and in dB. */',
        '#define SP1_AUDIO_USB_PARKED_STEP    %d' % cfg['parked_step'],
        '#define SP1_AUDIO_USB_PARKED_DB      (%d)' % cfg['parked_db'],
        '/* [usb] speaker: 1 = the speaker keeps playing while a host takes USB audio out. */',
        '#define SP1_AUDIO_USB_SPEAKER        %d' % cfg['speaker'],
        '',
        '/* struct sp1_audio_cfg (sp1_cnfg.h), field by field (#52). */',
        '#define SP1_AUDIO_CFG_DEFAULTS { %d, %d, %d, %d }'
        % (cfg['parked'], cfg['parked_db'], cfg['parked_step'], cfg['speaker']),
        '',
    ] + c_template('SP1_AUDIO_TEMPLATE', text) + [''])


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        sys.exit(2)
    src, out = sys.argv[1], sys.argv[2]
    with open(src, encoding='utf-8-sig') as f:
        raw = f.read().replace('\r\n', '\n')
    text = header(parse(src), 'config/audio.cnfg', raw)
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
