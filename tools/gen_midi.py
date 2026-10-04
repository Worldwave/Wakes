#!/usr/bin/env python3
"""wakes-sp1 -- generate the firmware's MIDI tables from the MIDI script, config/midi.ini.

    gen_midi.py <midi.ini> <out.h> [--chart <dir>]     build step (CMake runs this)

Two inputs, deliberately kept apart (as gen_engines.py does for engines.csv):

  config/midi.ini   the USER's choices: channel, note priority, legato, portamento, bend
                    range, the sustain pedal's CC, the CC smoothing time, which CC moves
                    which parameter, and optional velocity / aftertouch bindings.
  this file         FACTS the user cannot choose: which parameters exist, which thread
                    applies each one, and which CC numbers MIDI itself reserves (DESTS,
                    RESERVED).

The build fails, naming the line, on anything invalid -- an unknown name, a CC used twice,
a reserved CC, a value out of range. A script that half-works is worse than one that
refuses to build.

--chart also writes the CC chart for this script, next to the firmware:
  wakes-sp1-midi.md    for people
  wakes-sp1-midi.csv   midi.guide's column layout (github.com/pencilresearch/midi)

Standard library only. Read as UTF-8 (with or without a BOM).
"""
import configparser
import csv
import os
import re
import sys

# ---- facts: NOT user configuration ---------------------------------------------------------
# (section, key, label, kind, page)
#   kind  audio = continuous, applied and smoothed in the AUDIO thread (Plaits)
#         main  = continuous, applied in the control loop (Marbles reads its parameters
#                 once per 5 ms audio block, so audio-rate smoothing buys nothing there)
#         step  = stepped: applied before the parameter is quantized, never smoothed
# The ORDER is the enum order in the generated header; firmware/src/sp1_midi.cc and the
# two UI files use those names, so do not reorder or rename without changing them too.
DESTS = [
    ('plaits', 'frequency',              'FREQUENCY',              'audio', 'PLAITS BASE F1'),
    ('plaits', 'timbre',                 'TIMBRE',                 'audio', 'PLAITS BASE F2'),
    ('plaits', 'morph',                  'MORPH',                  'audio', 'PLAITS BASE F3'),
    ('plaits', 'harmonics',              'HARMONICS',              'audio', 'PLAITS BASE F4'),
    ('plaits', 'fm_attenuverter',        'FM attenuverter',        'audio', 'PLAITS SHIFT F1'),
    ('plaits', 'timbre_attenuverter',    'TIMBRE attenuverter',    'audio', 'PLAITS SHIFT F2'),
    ('plaits', 'morph_attenuverter',     'MORPH attenuverter',     'audio', 'PLAITS SHIFT F3'),
    ('plaits', 'harmonics_attenuverter', 'HARMONICS attenuverter', 'audio', 'PLAITS SHIFT F4'),
    ('plaits', 'lpg_colour',             'LPG colour',             'audio', 'PLAITS SETTINGS F2'),
    ('plaits', 'lpg_decay',              'LPG decay',              'audio', 'PLAITS SETTINGS F3'),
    ('plaits', 'level',                  'LEVEL',                  'audio', 'PLAITS SETTINGS F4'),
    ('plaits', 'octave_range',           'OCTAVE range',           'step',  'PLAITS SETTINGS F1'),
    ('plaits', 'model',                  'MODEL (engine)',         'step',  'PLAITS (no fader)'),
    ('marbles', 'rate',                  'RATE',                   'main',  'MARBLES t F1'),
    ('marbles', 't_bias',                't BIAS',                 'main',  'MARBLES t F2'),
    ('marbles', 'jitter',                'JITTER',                 'main',  'MARBLES t F3'),
    ('marbles', 'deja_vu',               'DEJA VU',                'main',  'MARBLES t/X F4'),
    ('marbles', 'gate_length',           'gate length',            'main',  'MARBLES t SHIFT F2'),
    ('marbles', 'gate_length_random',    'gate length randomness', 'main',  'MARBLES t SHIFT F3'),
    ('marbles', 'length',                'LENGTH',                 'step',  'MARBLES t SHIFT F4'),
    ('marbles', 'spread',                'SPREAD',                 'main',  'MARBLES X F1'),
    ('marbles', 'x_bias',                'X BIAS',                 'main',  'MARBLES X F2'),
    ('marbles', 'steps',                 'STEPS',                  'main',  'MARBLES X F3'),
    ('marbles', 'y_spread',              'Y SPREAD',               'main',  'MARBLES SETTINGS F1'),
    ('marbles', 'y_bias',                'Y BIAS',                 'main',  'MARBLES SETTINGS F2'),
    ('marbles', 'y_steps',               'Y STEPS',                'main',  'MARBLES SETTINGS F3'),
    ('marbles', 'y_divider',             'Y divider',              'step',  'MARBLES SETTINGS F4'),
]
KIND = {'audio': 0, 'main': 1, 'step': 2}

# ---- which way each CC reads: the INTELLIGENT rule (M4c), applied to MIDI (M5a test notes) ----
# Same policy as Marbles' INTELLIGENT [J] range (sp1_marbles.cc, IntelligentRange): a BIPOLAR
# parameter -- one whose centre is its neutral point -- takes a centred CC (64 = no change,
# 0 / 127 = half a fader's travel down / up: from a centred fader, exactly the two ends); a
# UNIPOLAR one takes a one-sided CC (0 = no change, 127 = a whole travel up: from a fader at 0,
# exactly the top). So from the fader's neutral position the whole CC range is usable, and a
# host knob at 0 leaves a one-sided parameter's fader alone. 'engine:<bit>' = decided per engine by the detent table, exactly as
# INTELLIGENT decides it for TIMBRE / MORPH / HARMONICS (SP1_ENGINE_TABLE[].centre bits:
# 0x1 HARMONICS, 0x2 TIMBRE, 0x4 MORPH). Not a new table: the bipolar faders are the ones with
# a centre detent (docs/PLAITS-ENGINES.md, sp1_plaits_ui.c, sp1_marbles_ui.c is_bipolar).
#   FREQUENCY, the attenuverters, RATE: bipolar by nature -- transpose, depth either way, and
#   Marbles' own RATE CV is +-5 V around 120 BPM.
#   MODEL, OCTAVE range, LENGTH, Y divider: selectors, one-sided like Plaits' own MODEL CV.
POLARITY = {
    'frequency': 'bi', 'timbre': 'engine:0x2', 'morph': 'engine:0x4', 'harmonics': 'engine:0x1',
    'fm_attenuverter': 'bi', 'timbre_attenuverter': 'bi', 'morph_attenuverter': 'bi',
    'harmonics_attenuverter': 'bi', 'lpg_colour': 'uni', 'lpg_decay': 'uni', 'level': 'uni',
    'octave_range': 'uni', 'model': 'uni',
    'rate': 'bi', 't_bias': 'bi', 'jitter': 'uni', 'deja_vu': 'bi', 'gate_length': 'uni',
    'gate_length_random': 'uni', 'length': 'uni', 'spread': 'uni', 'x_bias': 'bi',
    'steps': 'bi', 'y_spread': 'uni', 'y_bias': 'bi', 'y_steps': 'bi', 'y_divider': 'uni',
}
assert set(POLARITY) == {k for (_s, k, *_r) in DESTS}, 'POLARITY must cover every destination'


def polarity_code(k):
    """0 unipolar, 1 bipolar, 0x10 | centre bit = decided by the engine's detents."""
    v = POLARITY[k]
    if v == 'uni':
        return 0
    if v == 'bi':
        return 1
    return 0x10 | int(v.split(':')[1], 16)
RESERVED = {
    0: 'bank select', 32: 'bank select (fine)',
    6: 'data entry', 38: 'data entry (fine)',
    96: 'data increment', 97: 'data decrement',
    98: 'NRPN select', 99: 'NRPN select', 100: 'RPN select', 101: 'RPN select',
}
for n in range(120, 128):
    RESERVED[n] = 'channel mode message'
PRIORITY = {'last': 0, 'low': 1, 'high': 2}          # stmlib NoteStackFlags
LEGATO = {'off': 0, 'auto': 1, 'on': 2}              # Yarns' legato_mode
# How a CC and its fader share a parameter (Adara, M5a round 4). sp1_midi.h, "pickup".
PICKUP = {'sum': 0, 'shared': 1, 'takeover': 2}
SOURCES = ('velocity', 'aftertouch')


def fail(path, line, msg):
    where = '%s:%d' % (path, line) if line else path
    sys.stderr.write('\n%s: error: %s\n\n'
                     'The MIDI script is checked at build time so a mistake cannot reach\n'
                     'the device. See the comments in config/midi.ini.\n' % (where, msg))
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
    # interpolation=None: a depth is written "50%", and configparser's default
    # interpolation would read the % as the start of a %(name)s reference and crash.
    cp = configparser.ConfigParser(inline_comment_prefixes=(';', '#'),
                                   comment_prefixes=(';', '#'), strict=True,
                                   interpolation=None)
    try:
        cp.read_string(text, source=path)
    except configparser.Error as e:
        fail(path, getattr(e, 'lineno', 0), str(e).splitlines()[0])

    def err(sec, key, msg):
        fail(path, line_of(text, sec, key), msg)

    allowed = {'midi', 'plaits', 'marbles', 'bind'}
    for sec in cp.sections():
        if sec not in allowed:
            fail(path, 0, 'unknown section [%s] (expected %s)'
                 % (sec, ', '.join('[%s]' % s for s in sorted(allowed))))

    cfg = {'channel': 0, 'priority': 0, 'legato': 0, 'portamento': 0, 'bend_range': 2,
           'sustain': 64, 'smooth_ms': 10, 'pickup': PICKUP['shared'], 'clock': 1,
           'clock_lead': -1}
    m = cp['midi'] if cp.has_section('midi') else {}
    known = {'channel', 'note_priority', 'legato', 'portamento', 'bend_range', 'sustain',
             'cc_smoothing', 'pickup', 'clock', 'clock_lead'}
    for k in m:
        if k not in known:
            err('midi', k, 'unknown setting "%s" in [midi]' % k)
    v = m.get('channel', '1').strip().lower()
    if v == 'omni':
        cfg['channel'] = 16
    elif v.isdigit() and 1 <= int(v) <= 16:
        cfg['channel'] = int(v) - 1
    else:
        err('midi', 'channel', 'channel must be 1-16 or omni, not "%s"' % v)
    v = m.get('note_priority', 'last').strip().lower()
    if v not in PRIORITY:
        err('midi', 'note_priority', 'note_priority must be last, low or high')
    cfg['priority'] = PRIORITY[v]
    v = m.get('legato', 'off').strip().lower()
    if v not in LEGATO:
        err('midi', 'legato', 'legato must be off, on or auto')
    cfg['legato'] = LEGATO[v]
    v = m.get('portamento', '0').strip().lower()
    mt = re.fullmatch(r't(\d+)', v)
    mr = re.fullmatch(r'r(\d+)', v)
    if v == '0':
        cfg['portamento'] = 0
    elif mt and 1 <= int(mt.group(1)) <= 50:
        cfg['portamento'] = int(mt.group(1))            # Yarns: 1..50 constant time
    elif mr and 0 <= int(mr.group(1)) <= 50:
        cfg['portamento'] = 51 + int(mr.group(1))       # Yarns: 51..101 constant rate
    else:
        err('midi', 'portamento', 'portamento must be 0, t1-t50 or r0-r50, not "%s"' % v)
    v = m.get('bend_range', '2').strip()
    if not (v.isdigit() and 0 <= int(v) <= 24):
        err('midi', 'bend_range', 'bend_range must be 0-24 semitones')
    cfg['bend_range'] = int(v)
    v = m.get('cc_smoothing', '10').strip()
    if not (v.isdigit() and 0 <= int(v) <= 200):
        err('midi', 'cc_smoothing', 'cc_smoothing must be 0-200 ms')
    cfg['smooth_ms'] = int(v)
    v = m.get('pickup', 'shared').strip().lower()
    if v not in PICKUP:
        err('midi', 'pickup', 'pickup must be sum, shared or takeover, not "%s"' % v)
    cfg['pickup'] = PICKUP[v]
    v = m.get('clock', 'on').strip().lower()
    if v not in ('on', 'off'):
        err('midi', 'clock', 'clock must be on or off, not "%s"' % v)
    cfg['clock'] = 1 if v == 'on' else 0
    v = m.get('clock_lead', 'auto').strip().lower()
    if v == 'auto':
        cfg['clock_lead'] = -1
    elif v == 'notes':                      # #32: learnt from the host's quantised notes
        cfg['clock_lead'] = -2
    elif v.isdigit() and int(v) <= 200:
        cfg['clock_lead'] = int(v)
    else:
        err('midi', 'clock_lead', 'clock_lead must be notes, auto or 0-200 (ms), not "%s"' % v)

    def cc_value(sec, key, raw, allow_none):
        raw = raw.strip().lower()
        if allow_none and raw in ('none', 'off', ''):
            return -1
        mm = re.fullmatch(r'cc\s*(\d+)', raw)
        if not mm or not (0 <= int(mm.group(1)) <= 127):
            err(sec, key, '"%s" is not "cc N" with N 0-127%s'
                % (raw, ' (or none)' if allow_none else ''))
        n = int(mm.group(1))
        if n in RESERVED:
            err(sec, key, 'CC %d is reserved by MIDI (%s)' % (n, RESERVED[n]))
        return n

    v = m.get('sustain', 'cc 64')
    cfg['sustain'] = cc_value('midi', 'sustain', v, True)
    if 33 <= cfg['sustain'] <= 63:
        err('midi', 'sustain', 'CC %d is the fine half of a 14-bit pair' % cfg['sustain'])

    names = {(s, k): i for i, (s, k, *_rest) in enumerate(DESTS)}
    ccs = [-1] * len(DESTS)
    used = {}
    if cfg['sustain'] >= 0:
        used[cfg['sustain']] = 'the sustain pedal'
    for sec in ('plaits', 'marbles'):
        if not cp.has_section(sec):
            continue
        for k, raw in cp[sec].items():
            if (sec, k) not in names:
                err(sec, k, 'no parameter "%s" in [%s] (see the list in config/midi.ini)'
                    % (k, sec))
            n = cc_value(sec, k, raw, True)
            if n < 0:
                continue
            if 33 <= n <= 63:
                err(sec, k, 'CC %d is the fine half of 14-bit CC %d; use CC %d' % (n, n - 32, n - 32))
            if n in used:
                err(sec, k, 'CC %d already moves %s' % (n, used[n]))
            if n < 32 and (n + 32) in used:
                err(sec, k, 'CC %d is 14-bit and its fine half, CC %d, is already used'
                    % (n, n + 32))
            used[n] = '%s %s' % (sec, k)
            ccs[names[(sec, k)]] = n

    binds = {}
    b = cp['bind'] if cp.has_section('bind') else {}
    for k, raw in b.items():
        if k not in SOURCES:
            err('bind', k, 'unknown source "%s" (expected velocity or aftertouch)' % k)
        mm = re.fullmatch(r'([a-z_]+)(?:\s+(-?\d+)\s*%)?', raw.strip().lower())
        if not mm:
            err('bind', k, '"%s" is not "<parameter> [depth%%]"' % raw.strip())
        target = mm.group(1)
        hits = [i for (s, kk), i in names.items() if kk == target]
        if not hits:
            err('bind', k, 'no parameter "%s"' % target)
        depth = int(mm.group(2)) if mm.group(2) else 100
        if not -100 <= depth <= 100:
            err('bind', k, 'depth must be -100%% .. 100%%')
        binds[k] = (hits[0], depth / 100.0)
    return cfg, ccs, binds


def generate(ini, out, chart_dir):
    cfg, ccs, binds = parse(ini)
    enum = ['SP1_MIDI_D_%s' % k.upper() for (_s, k, *_r) in DESTS]
    cc_dest = [-1] * 128
    for i, n in enumerate(ccs):
        if n >= 0:
            cc_dest[n] = i
    L = ['/* GENERATED by tools/gen_midi.py from config/midi.ini -- do not edit. */',
         '#ifndef SP1_MIDI_GEN_H', '#define SP1_MIDI_GEN_H', '', '#include <stdint.h>', '',
         '#define SP1_MIDI_OMNI        16',
         '#define SP1_MIDI_CHANNEL     %d   /* 0-15, or SP1_MIDI_OMNI */' % cfg['channel'],
         '#define SP1_MIDI_PRIORITY    %d   /* stmlib NoteStackFlags: 0 last, 1 low, 2 high */'
         % cfg['priority'],
         '#define SP1_MIDI_LEGATO      %d   /* Yarns legato_mode: 0 off, 1 auto, 2 on */'
         % cfg['legato'],
         '#define SP1_MIDI_PORTAMENTO  %d   /* Yarns: 0 none, 1-50 time, 51-101 rate */'
         % cfg['portamento'],
         '#define SP1_MIDI_BEND_RANGE  %d   /* semitones, until RPN 0 says otherwise */'
         % cfg['bend_range'],
         '#define SP1_MIDI_SUSTAIN_CC  (%d)   /* -1 = no sustain pedal */' % cfg['sustain'],
         '#define SP1_MIDI_SMOOTH_MS   %d' % cfg['smooth_ms'],
         '#define SP1_MIDI_PICKUP_SUM      0   /* CC = an offset on top of the fader */',
         '#define SP1_MIDI_PICKUP_SHARED   1   /* CC and fader move ONE value, both catch up */',
         '#define SP1_MIDI_PICKUP_TAKEOVER 2   /* port up: the CC moves it, its fader rests */',
         '#define SP1_MIDI_PICKUP      %d' % cfg['pickup'],
         '#define SP1_MIDI_CLOCK       %d   /* 1 = Marbles follows MIDI clock + transport */'
         % cfg['clock'],
         '#define SP1_MIDI_CLOCK_LEAD_MS (%d)   /* ms; -1 = auto: Wakes\' own delay; -2 = notes */'
         % cfg['clock_lead'], '',
         'enum sp1_midi_dest {']
    L += ['\t%s,' % e for e in enum]
    L += ['\tSP1_MIDI_DESTS', '};', '',
          '/* kind: 0 audio thread (smoothed per Plaits block), 1 control loop (smoothed per',
          ' * tick), 2 stepped (applied before quantizing, not smoothed). */',
          '#define SP1_MIDI_K_AUDIO 0', '#define SP1_MIDI_K_MAIN  1', '#define SP1_MIDI_K_STEP  2',
          '']
    for src in SOURCES:
        d, depth = binds.get(src, (-1, 0.0))
        L.append('#define SP1_MIDI_%s_DEST  (%d)' % (src.upper(), d))
        L.append('#define SP1_MIDI_%s_DEPTH (%sf)' % (src.upper(), repr(float(depth))))
    L += ['', '#ifdef SP1_MIDI_GEN_TABLES',
          'static const uint8_t SP1_MIDI_KIND[SP1_MIDI_DESTS] = {']
    L += ['\t%d,   /* %s */' % (KIND[kind], enum[i]) for i, (_s, _k, _l, kind, _p) in enumerate(DESTS)]
    L += ['};', '/* 0 unipolar, 1 bipolar, 0x10 | SP1_ENGINE_TABLE[].centre bit = per engine. */',
          'static const uint8_t SP1_MIDI_POLARITY[SP1_MIDI_DESTS] = {']
    L += ['\t0x%02x,   /* %s: %s */' % (polarity_code(k), enum[i], POLARITY[k])
          for i, (_s, k, *_r) in enumerate(DESTS)]
    L += ['};', 'static const char *const SP1_MIDI_NAME[SP1_MIDI_DESTS] = {']
    L += ['\t"%s %s",' % (s, k) for (s, k, *_r) in DESTS]
    L += ['};', '/* destination -> its CC number (the coarse half), or -1 = none. */',
          'static const int8_t SP1_MIDI_DEST_CC[SP1_MIDI_DESTS] = {']
    L += ['\t%d,   /* %s */' % (n, enum[i]) for i, n in enumerate(ccs)]
    L += ['};', '/* CC number (the coarse half, for 0-31) -> destination, or -1. */',
          'static const int8_t SP1_MIDI_CC_DEST[128] = {']
    for r in range(0, 128, 16):
        L.append('\t' + ' '.join('%d,' % x for x in cc_dest[r:r + 16]))
    L += ['};', '#endif /* SP1_MIDI_GEN_TABLES */', '', '#endif /* SP1_MIDI_GEN_H */', '']
    write_if_changed(out, '\n'.join(L))
    if chart_dir:
        write_charts(chart_dir, cfg, ccs, binds)


def write_if_changed(path, text):
    old = None
    if os.path.exists(path):
        with open(path, encoding='utf-8', newline='') as f:
            old = f.read()
    if old != text:
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)


def write_charts(d, cfg, ccs, binds):
    ch = 'omni' if cfg['channel'] == 16 else str(cfg['channel'] + 1)
    summing = cfg['pickup'] == PICKUP['sum']
    how = {
        PICKUP['sum']: [
            '- Pickup **sum**: every CC is an **offset** on its fader. **centred** parameters:',
            '  64 = no change, 0 / 127 = half a travel down / up (from a centred fader: the two',
            '  ends). **one-sided** parameters: 0 = no change, 127 = a whole travel up (from a',
            '  fader at 0: the top). **per engine**: centred when that engine gives the fader a',
            '  centre detent, one-sided otherwise.'],
        PICKUP['shared']: [
            '- Pickup **shared**: a CC moves its parameter exactly as its fader does, 0 = the',
            '  bottom, 127 = the top. Fader and CC share the one value: whichever you move takes',
            "  it from where it is and catches up with your hand, as Plaits' knobs do."],
        PICKUP['takeover']: [
            '- Pickup **takeover**: while MIDI is plugged in, the faders that have a CC below',
            '  rest and the CC moves the parameter, 0 = the bottom, 127 = the top, catching up',
            '  with the value the fader left. Unplug and the fader catches up in turn.'],
    }[cfg['pickup']]
    md = ['# Wakes — MIDI CC chart', '',
          'Generated from `config/midi.ini` by the build that produced this firmware.', '',
          '- Channel: **%s**' % ch,
          '- Notes play Plaits; pitch bend ±%d semitones until the host sends RPN 0'
          % cfg['bend_range'],
          '- Sustain pedal: %s' % ('CC %d' % cfg['sustain'] if cfg['sustain'] >= 0 else 'off'),
          '- MIDI clock: %s' % (('Marbles follows the host\'s clock, Start / Continue / Stop; '
                                 'RATE picks the ratio (1/4 … 4); running %s ahead'
                                 % ("Wakes' own delay (auto)" if cfg['clock_lead'] == -1
                                    else "as far as the host's notes say (notes)"
                                    if cfg['clock_lead'] == -2
                                    else '%d ms' % cfg['clock_lead']))
                                if cfg['clock'] else 'ignored')]
    md += how
    md += ['- MODEL is always an offset from the engine T2/T3 selected (it has no fader).',
           '- CC 0–31 are 14-bit (fine half on N+32).', '']
    md += (['| CC | fine | parameter | reads | where |', '|---|---|---|---|---|'] if summing
           else ['| CC | fine | parameter | where |', '|---|---|---|---|'])
    rows = sorted((n, i) for i, n in enumerate(ccs) if n >= 0)
    for n, i in rows:
        _s, _k, label, kind, page = DESTS[i]
        fine = str(n + 32) if n < 32 else ''
        if summing:
            reads = {'uni': 'one-sided', 'bi': 'centred'}.get(POLARITY[_k], 'per engine')
            md.append('| %d | %s | %s | %s | %s |' % (n, fine, label, reads, page))
        else:
            md.append('| %d | %s | %s | %s |' % (n, fine, label,
                                                 page + (' (offset)' if _k == 'model' else '')))
    unbound = [DESTS[i][2] for i, n in enumerate(ccs) if n < 0]
    if unbound:
        md += ['', 'No CC: ' + ', '.join(unbound)]
    for src in SOURCES:
        if src in binds:
            i, depth = binds[src]
            md.append('- %s → %s, depth %d %%' % (src, DESTS[i][2], round(depth * 100)))
    write_if_changed(os.path.join(d, 'wakes-sp1-midi.md'), '\n'.join(md) + '\n')

    hdr = ['manufacturer', 'device', 'section', 'parameter_name', 'parameter_description',
           'cc_msb', 'cc_lsb', 'cc_min_value', 'cc_max_value', 'cc_default_value',
           'nrpn_msb', 'nrpn_lsb', 'nrpn_min_value', 'nrpn_max_value', 'nrpn_default_value',
           'orientation', 'notes', 'usage']
    import io
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator='\n')
    w.writerow(hdr)
    for n, i in rows:
        s, _k, label, kind, page = DESTS[i]
        stepped = kind == 'step'
        pol = POLARITY[_k]
        centred = pol == 'bi'
        if summing or _k == 'model':
            note = ('64 = no offset' if centred else
                    '0 = no offset' if pol == 'uni' else
                    '64 = no offset on engines where this fader has a centre detent; 0 otherwise')
            note += '; offsets add to the fader' if _k != 'model' else \
                '; an offset from the engine T2/T3 selected'
            desc = 'Offset on %s (%s)' % (label, page)
            usage = '0~127: %s offset' % ('Stepped' if stepped else 'Continuous')
        else:
            note = ('Moves %s as its fader does; fader and CC share the value (pickup %s)'
                    % (label, 'shared' if cfg['pickup'] == PICKUP['shared'] else 'takeover'))
            desc = '%s (%s)' % (label, page)
            usage = '0~127: %s' % ('Stepped' if stepped else 'Continuous')
        w.writerow(['Worldwave', 'Wakes', s.capitalize(), label, desc,
                    n, (n + 32) if n < 32 else '',
                    0, 127, 64 if centred else 0, '', '', '', '', '',
                    'centered' if centred else '0-based',
                    'Community firmware for the SP-1. %s.' % note, usage])
    if cfg['sustain'] >= 0:
        w.writerow(['Worldwave', 'Wakes', 'Notes', 'Sustain pedal', 'Holds released notes',
                    cfg['sustain'], '', 0, 127, 0, '', '', '', '', '', '0-based', '',
                    '0-63: Off; 64-127: On'])
    write_if_changed(os.path.join(d, 'wakes-sp1-midi.csv'), buf.getvalue())


def main():
    args = sys.argv[1:]
    chart = None
    if '--chart' in args:
        k = args.index('--chart')
        chart = args[k + 1]
        del args[k:k + 2]
    if len(args) != 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    generate(args[0], args[1], chart)


if __name__ == '__main__':
    main()
