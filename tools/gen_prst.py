#!/usr/bin/env python3
"""wakes-sp1 -- generate the PRST preset format's field table (M6, #50).

    gen_prst.py <out.h>                       build step (CMake runs this)
    gen_prst.py --fingerprint                 print the current field list's fingerprint

A PRST slot is a `.prst` file of ini text (src/sp1_prst.h). This file is the ONE list of what
goes in it: every key, its section, what kind of value it holds, and where in
`struct sp1_prst` it lands. sp1_prst.c writes and reads files from the generated table; it
holds no list of its own.

---- the format version (Adara, #50) ----
"Every slot carries a format version. Firmware that doesn't recognise a slot's version starts
that slot from defaults. The build fails if the format changes without a version bump."

So the field list has a FINGERPRINT (a hash of every key, kind, check and choice token), and
FORMATS below records the fingerprint of each released version. The build fails if the
current list's fingerprint is not the newest entry. To change the format on purpose: add
the next version with the fingerprint the error prints -- and the release that ships it
must say so and ask users to install wakes-sp1-fresh.

⚠️ Never edit an existing FORMATS entry: a version names one exact format forever.

Standard library only.
"""
import hashlib
import os
import sys

# ---- the format versions: {version: fingerprint}. Append only. ------------------------------
FORMATS = {
    1: '83265ab33880809b',
}

# ---- choices: (token written in the file, C expression of the value) ----------------------
# Tokens are lowercase and space-free so a preset stays easy to edit by hand; reading ignores
# case. The VALUE is a C expression -- an enum name where one exists -- so the table cannot
# drift from the firmware's enum order.
DESTS = [
    ('none', 'SP1_DEST_NONE'), ('fm', 'SP1_DEST_FM'), ('timbre', 'SP1_DEST_TIMBRE'),
    ('morph', 'SP1_DEST_MORPH'), ('harmonics', 'SP1_DEST_HARM'), ('voct', 'SP1_DEST_VOCT'),
    ('level', 'SP1_DEST_LEVEL'), ('trig', 'SP1_DEST_TRIG'),
]
SCALES = [
    ('major', '0'), ('minor', '1'), ('pentatonic', '2'), ('pelog', '3'), ('bhairav', '4'),
    ('shri', '5'), ('free', '6'),
]
CHOICES = {
    'dest':      DESTS,
    'scale':     SCALES,
    'scale_off': [('off', 'SP1_PUI_SCALE_OFF')] + SCALES,
    'output':    [('out', 'SP1_OUT_MAIN'), ('aux', 'SP1_OUT_AUX'), ('out+aux', 'SP1_OUT_SUM'),
                  ('outxaux', 'SP1_OUT_RING')],
    'burst':     [('1/%d' % (1 << i), str(i)) for i in range(8)],         # g_burst_div 0..7
    'drive':     [('off', '0'), ('+3db', '1'), ('+8db', '2'), ('+15db', '3'), ('+24db', '4')],
    't_model':   [('bernoulli', '0'), ('clusters', '1'), ('drums', '2'),
                  ('independent_bernoulli', '3'), ('divider', '4'), ('three_states', '5')],
    't_range':   [('x0.25', '0'), ('x1', '1'), ('x4', '2')],
    'range':     [('0-2v', 'SP1_MRB_RANGE_NARROW'), ('0-5v', 'SP1_MRB_RANGE_POSITIVE'),
                  ('+-5v', 'SP1_MRB_RANGE_FULL'), ('intelligent', 'SP1_MRB_RANGE_INTELLIGENT')],
    'diversity': [('identical', '0'), ('bump', '1'), ('tilt', '2')],
    'onoff':     [('off', '0'), ('on', '1')],
}

# ---- the fields, in the order they are written -------------------------------------------
# (section, key, kind, check, path in struct sp1_prst)
#   kind  engine   = an engine NAME (config/engines.csv), resolved to its slot
#         pos      = a fader value, 0..1 (pot space, before detents)
#         <choice> = one of CHOICES[kind]
#   check extra validity beyond "is one of the tokens", applied by sp1_prst.c:
#         scale    = a scale that is included (docs/MARBLES-SETTINGS.md); scale_off also -1
#         t_dest / x_dest = on that side's destination ring
# Fader keys use config/midi.ini's names, so the two files speak the same vocabulary.
T, TS, XB, Y = 'SP1_MUI_T_BASE', 'SP1_MUI_T_SHIFT', 'SP1_MUI_X_BASE', 'SP1_MUI_Y'
B, SH, ST = 'SP1_PUI_BASE', 'SP1_PUI_SHIFT', 'SP1_PUI_SETTINGS'
FIELDS = [
    ('plaits', 'engine',                 'engine',    '',       'plaits.slot'),
    ('plaits', 'frequency',              'pos',       '',       'plaits.v[%s][0]' % B),
    ('plaits', 'timbre',                 'pos',       '',       'plaits.v[%s][1]' % B),
    ('plaits', 'morph',                  'pos',       '',       'plaits.v[%s][2]' % B),
    ('plaits', 'harmonics',              'pos',       '',       'plaits.v[%s][3]' % B),
    ('plaits', 'fm_attenuverter',        'pos',       '',       'plaits.v[%s][0]' % SH),
    ('plaits', 'timbre_attenuverter',    'pos',       '',       'plaits.v[%s][1]' % SH),
    ('plaits', 'morph_attenuverter',     'pos',       '',       'plaits.v[%s][2]' % SH),
    ('plaits', 'harmonics_attenuverter', 'pos',       '',       'plaits.v[%s][3]' % SH),
    ('plaits', 'octave_range',           'pos',       '',       'plaits.v[%s][0]' % ST),
    ('plaits', 'lpg_colour',             'pos',       '',       'plaits.v[%s][1]' % ST),
    ('plaits', 'lpg_decay',              'pos',       '',       'plaits.v[%s][2]' % ST),
    ('plaits', 'level',                  'pos',       '',       'plaits.v[%s][3]' % ST),
    ('plaits', 'quantizer',              'scale_off', 'scale',  'plaits.scale'),
    ('plaits', 'output',                 'output',    '',       'out_mode'),
    ('plaits', 'burst',                  'burst',     '',       'burst_div'),
    ('plaits', 'drive',                  'drive',     '',       'drive'),
    ('marbles', 'rate',                  'pos',       '',       'marbles.v[%s][0]' % T),
    ('marbles', 't_bias',                'pos',       '',       'marbles.v[%s][1]' % T),
    ('marbles', 'jitter',                'pos',       '',       'marbles.v[%s][2]' % T),
    ('marbles', 'deja_vu',               'pos',       '',       'marbles.v[%s][3]' % T),
    ('marbles', 'gtlt',                  'pos',       '',       'marbles.v[%s][0]' % TS),
    ('marbles', 'gate_length',           'pos',       '',       'marbles.v[%s][1]' % TS),
    ('marbles', 'gate_length_random',    'pos',       '',       'marbles.v[%s][2]' % TS),
    ('marbles', 'length',                'pos',       '',       'marbles.v[%s][3]' % TS),
    ('marbles', 'spread',                'pos',       '',       'marbles.v[%s][0]' % XB),
    ('marbles', 'x_bias',                'pos',       '',       'marbles.v[%s][1]' % XB),
    ('marbles', 'steps',                 'pos',       '',       'marbles.v[%s][2]' % XB),
    ('marbles', 'y_spread',              'pos',       '',       'marbles.v[%s][0]' % Y),
    ('marbles', 'y_bias',                'pos',       '',       'marbles.v[%s][1]' % Y),
    ('marbles', 'y_steps',               'pos',       '',       'marbles.v[%s][2]' % Y),
    ('marbles', 'y_divider',             'pos',       '',       'marbles.v[%s][3]' % Y),
    ('marbles', 't_model',               't_model',   '',       'marbles.model'),
    ('marbles', 't_range',               't_range',   '',       'marbles.t_range'),
    ('marbles', 'range',                 'range',     '',       'marbles.range'),
    ('marbles', 'diversity',             'diversity', '',       'marbles.diversity'),
    ('marbles', 'scale',                 'scale',     'scale',  'marbles.scale'),
    ('marbles', 'deja_vu_t',             'onoff',     '',       'marbles.dv_t'),
    ('marbles', 'deja_vu_x',             'onoff',     '',       'marbles.dv_x'),
    ('marbles', 't1',                    'dest',      't_dest', 'marbles.t_dest[0]'),
    ('marbles', 't2',                    'dest',      't_dest', 'marbles.t_dest[1]'),
    ('marbles', 't3',                    'dest',      't_dest', 'marbles.t_dest[2]'),
    ('marbles', 'x1',                    'dest',      'x_dest', 'marbles.dest[0]'),
    ('marbles', 'x2',                    'dest',      'x_dest', 'marbles.dest[1]'),
    ('marbles', 'x3',                    'dest',      'x_dest', 'marbles.dest[2]'),
    ('marbles', 'y',                     'dest',      'x_dest', 'marbles.dest[3]'),
]
CHECKS = ['', 'scale', 't_dest', 'x_dest']


def fail(msg):
    sys.stderr.write('gen_prst.py: %s\n' % msg)
    sys.exit(1)


def validate():
    seen = set()
    for sec, key, kind, check, path in FIELDS:
        if (sec, key) in seen:
            fail('%s.%s listed twice' % (sec, key))
        seen.add((sec, key))
        if kind not in ('engine', 'pos') and kind not in CHOICES:
            fail('%s.%s: unknown kind %r' % (sec, key, kind))
        if check not in CHECKS:
            fail('%s.%s: unknown check %r' % (sec, key, check))
        if not key.isidentifier() or key != key.lower():
            fail('%s.%s: keys are lowercase identifiers' % (sec, key))
    for name, toks in CHOICES.items():
        t = [a for a, _ in toks]
        if len(set(t)) != len(t):
            fail('choice %s has a token twice' % name)
        for a in t:
            if a != a.lower() or any(c.isspace() for c in a) or not a:
                fail('choice %s: token %r must be lowercase, without spaces' % (name, a))


def fingerprint():
    # Everything that decides what a file means. NOT the struct paths: they are where a
    # value lands in RAM, which can change without changing a single file.
    parts = []
    for sec, key, kind, check, _ in FIELDS:
        parts.append('%s.%s:%s:%s' % (sec, key, kind, check))
    for name in sorted(CHOICES):
        parts.append('%s=%s' % (name, ','.join('%s>%s' % tv for tv in CHOICES[name])))
    return hashlib.sha256('\n'.join(parts).encode('utf-8')).hexdigest()[:16]


def check_version():
    fp = fingerprint()
    newest = max(FORMATS)
    if FORMATS[newest] != fp:
        if fp in FORMATS.values():
            fail('the field list matches an OLDER format; restore it or add a new version')
        fail('the PRST format changed without a version bump.\n'
             '  Fingerprint now: %s (format %d is %s).\n'
             '  If the change is intended, add  %d: %r,  to FORMATS in tools/gen_prst.py --\n'
             '  and the release that ships it must say so and ask users to install\n'
             '  wakes-sp1-fresh (#50).' % (fp, newest, FORMATS[newest], newest + 1, fp))
    return newest, fp


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def generate(out):
    version, fp = check_version()
    L = []
    L.append('/* GENERATED by tools/gen_prst.py -- the PRST field table (#50).')
    L.append(' * Do not edit: edit FIELDS / CHOICES there. Included by sp1_prst.c only. */')
    L.append('#ifndef SP1_PRST_GEN_H')
    L.append('#define SP1_PRST_GEN_H')
    L.append('')
    L.append('#define SP1_PRST_FORMAT      %d' % version)
    L.append('#define SP1_PRST_FINGERPRINT "%s"' % fp)
    L.append('#define SP1_PRST_NFIELDS     %d' % len(FIELDS))
    L.append('')
    L.append('enum { SP1_PRST_K_ENGINE = 0, SP1_PRST_K_POS, SP1_PRST_K_CHOICE };')
    L.append('enum { %s };' % ', '.join('SP1_PRST_C_%s = %d' % ((c or 'none').upper(), i)
                                       for i, c in enumerate(CHECKS)))
    L.append('')
    for name in sorted(CHOICES):
        L.append('static const struct sp1_prst_choice sp1_prst_ch_%s[] = {' % name)
        for tok, val in CHOICES[name]:
            L.append('\t{ %s, %s },' % (c_str(tok), val))
        L.append('};')
    L.append('')
    L.append('static const struct sp1_prst_field SP1_PRST_FIELDS[SP1_PRST_NFIELDS] = {')
    for sec, key, kind, check, path in FIELDS:
        if kind in ('engine', 'pos'):
            k, ch, n = 'SP1_PRST_K_' + kind.upper(), 'NULL', '0'
        else:
            k = 'SP1_PRST_K_CHOICE'
            ch = 'sp1_prst_ch_' + kind
            n = 'sizeof(%s) / sizeof(%s[0])' % (ch, ch)
        L.append('\t{ %s, %s, %s, SP1_PRST_C_%s, %s, %s, offsetof(struct sp1_prst, %s) },'
                 % (c_str(sec), c_str(key), k, (check or 'none').upper(), ch, n, path))
    L.append('};')
    L.append('')
    L.append('#endif /* SP1_PRST_GEN_H */')
    text = '\n'.join(L) + '\n'
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    old = None
    if os.path.exists(out):
        with open(out, encoding='utf-8') as f:
            old = f.read()
    if old != text:                      # leave the timestamp alone when nothing changed
        with open(out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    print('PRST format %d (%s), %d fields -> %s' % (version, fp, len(FIELDS), out))


def main(argv):
    validate()
    if len(argv) == 2 and argv[1] == '--fingerprint':
        print(fingerprint())
        return
    if len(argv) != 2:
        fail('usage: gen_prst.py <out.h> | --fingerprint')
    generate(argv[1])


if __name__ == '__main__':
    main(sys.argv)
