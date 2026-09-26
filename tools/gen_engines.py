#!/usr/bin/env python3
"""wakes-sp1 -- generate the firmware's engine table from config/engines.csv.

    gen_engines.py <engines.csv> <out.h>     build step (CMake runs this)
    gen_engines.py --list                    print every engine a config may name

Two inputs, deliberately kept apart:

  config/engines.csv   the USER's choice: which engines are in the firmware, in which
                       slot, with which glyph. Columns Slot, Glyph, Engine. Leaving an
                       engine out removes it from T2/T3 engine select.
  PLAITS_ENGINES       below, in this file: FACTS about Plaits that a user cannot
                       choose -- each engine's index inside Plaits and its centre
                       detents. Keyed by name; the CSV only names engines.

Glyphs are four symbols, T1 first, each one of
    U+25CF  full     or  #
    U+25D0  half     or  +
    U+25CB  off      or  .
The ASCII aliases exist for editors and spreadsheets that mangle the circles. Every
glyph in a config must be different, so each engine is recognisable on its own.

Standard library only. The CSV is read as UTF-8 (with or without a BOM, which Excel
writes): Windows' default code page would mangle the glyphs.
"""
import csv
import os
import sys

# ---- facts about Plaits: NOT user configuration ----------------------------------------
# (index in plaits/dsp/voice.cc, name, centre detents). The index is the engine's identity
# inside Plaits, fixed by upstream. A detent marks a fader whose centre is an exact neutral
# point, which also makes it the per-engine bipolar table M4c's INTELLIGENT range reads
# (SP1_ENGINE_TABLE[].centre). The reasoning for each is in docs/PLAITS-ENGINES.md, "Why
# these detents". Change these only when Plaits itself changes.
PLAITS_ENGINES = [
    (0,  "VA + VCF",         "F3 F4"),
    (1,  "phase distortion", ""),
    (2,  "6-op FM A",        "F3"),
    (3,  "6-op FM B",        "F3"),
    (4,  "6-op FM C",        "F3"),
    (5,  "wave terrain",     "F3"),
    (6,  "string machine",   "F2"),
    (7,  "chiptune",         ""),
    (8,  "virtual analog",   "F2 F4"),
    (9,  "waveshaping",      "F4"),
    (10, "2-op FM",          "F3"),
    (11, "grain / formant",  "F4"),
    (12, "additive",         ""),
    (13, "wavetable",        ""),
    (14, "chords",           ""),
    (15, "speech",           ""),
    (16, "swarm",            ""),
    (17, "filtered noise",   "F4"),
    (18, "particle",         "F3"),
    (19, "string",           ""),
    (20, "modal",            ""),
    (21, "bass drum",        ""),
    (22, "snare drum",       ""),
    (23, "hi-hat",           ""),
]
DETENT_BITS = {"F4": 0x1, "F2": 0x2, "F3": 0x4}   # HARMONICS, TIMBRE, MORPH

GLYPH_LEVEL = {"○": 0, ".": 0, "◐": 1, "+": 1, "●": 2, "#": 2}
COLUMNS = ("slot", "glyph", "engine")
CSV_NAME = "config/engines.csv"


def fail(msg):
    msg = "\n*** %s: %s\n\n" % (CSV_NAME, msg)
    try:
        msg.encode(sys.stderr.encoding or "ascii")
    except UnicodeEncodeError:
        # A Windows console (cp1252) cannot show the circles; the ASCII forms can.
        msg = msg.translate({ord("●"): "#", ord("◐"): "+", ord("○"): "."})
    sys.stderr.write(msg)
    sys.exit(1)


def facts():
    by_name = {}
    for idx, name, det in PLAITS_ENGINES:
        bits = 0
        for d in det.split():
            bits |= DETENT_BITS[d]
        by_name[name.lower()] = dict(plaits=idx, name=name, centre=bits)
    assert sorted(e["plaits"] for e in by_name.values()) == list(range(24))
    return by_name


def glyph_levels(text, where):
    syms = [ch for ch in text if not ch.isspace()]
    if len(syms) != 4 or any(ch not in GLYPH_LEVEL for ch in syms):
        fail("%s: Glyph must be exactly four symbols, T1 first: ● full, ◐ half, ○ off\n"
             "    (or # + .) -- got `%s`. If the circles came out as '?' or garbage, your\n"
             "    editor saved in the wrong encoding: save as UTF-8, or use # + . instead."
             % (where, text))
    return [GLYPH_LEVEL[ch] for ch in syms]


def parse(csv_path):
    known = facts()
    with open(csv_path, encoding="utf-8-sig", newline="") as f:
        rows = list(csv.reader(f))
    rows = [(n + 1, r) for n, r in enumerate(rows) if any(c.strip() for c in r)]
    if not rows:
        fail("the file is empty")
    header = [c.strip().lower() for c in rows[0][1]]
    if tuple(header[:3]) != COLUMNS:
        fail("line %d: the first row must be the header Slot,Glyph,Engine -- got %s"
             % (rows[0][0], ",".join(rows[0][1])))

    engines, by_slot, by_engine, by_glyph = [], {}, {}, {}
    for line, r in rows[1:]:
        if len(r) < 3:
            fail("line %d: expected Slot,Glyph,Engine -- got %s" % (line, ",".join(r)))
        slot_t, glyph_t, name_t = (c.strip() for c in r[:3])
        where = "line %d (%s)" % (line, name_t or "no engine")
        e = known.get(name_t.lower())
        if e is None:
            fail("%s: no engine is called %r. Available engines:\n    %s"
                 % (where, name_t, "\n    ".join(n for _, n, _ in PLAITS_ENGINES)))
        if e["name"] in by_engine:
            fail("%s: %s is already in slot %d; an engine can be listed once"
                 % (where, e["name"], by_engine[e["name"]]))
        try:
            slot = int(slot_t)
        except ValueError:
            fail("%s: Slot must be a number, got %r" % (where, slot_t))
        if slot in by_slot:
            fail("%s: slot %d is also %s's" % (where, slot, by_slot[slot]))
        levels = glyph_levels(glyph_t, where)
        if tuple(levels) in by_glyph:
            fail("%s: glyph %s is the same as %s's -- every glyph must be different"
                 % (where, glyph_t, by_glyph[tuple(levels)]))
        by_slot[slot], by_engine[e["name"]], by_glyph[tuple(levels)] = \
            e["name"], slot, e["name"]
        engines.append(dict(e, slot=slot, levels=levels))

    if not engines:
        fail("no engines listed; the firmware needs at least one")
    engines.sort(key=lambda e: e["slot"])
    want = list(range(1, len(engines) + 1))
    if [e["slot"] for e in engines] != want:
        fail("slots must be numbered 1 to %d with none missing -- got %s"
             % (len(engines), ", ".join(str(e["slot"]) for e in engines)))
    return engines


def c_str(s):
    s = s.encode("ascii", "replace").decode("ascii")[:24]
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def generate(csv_path, out_path):
    engines = parse(csv_path)
    out = [
        "/* GENERATED from config/engines.csv by tools/gen_engines.py.",
        " * Do not edit: edit the CSV and rebuild. */",
        "#ifndef SP1_ENGINES_GEN_H",
        "#define SP1_ENGINES_GEN_H",
        "",
        "#include <stdint.h>",
        "",
        "#define SP1_ENGINE_SLOTS        %d" % len(engines),
        "#define SP1_ENGINE_DEFAULT_SLOT 0   /* slot 1: the device starts here */",
        "",
        "/* centre: 0x1 = F4 HARMONICS, 0x2 = F2 TIMBRE, 0x4 = F3 MORPH",
        " * led:    T1..T4, 0 = off, 1 = half, 2 = full */",
        "static const struct {",
        "\tuint8_t plaits;     /* index in plaits/dsp/voice.cc */",
        "\tuint8_t centre;",
        "\tuint8_t led[4];",
        "\tconst char *name;",
        "} SP1_ENGINE_TABLE[SP1_ENGINE_SLOTS] = {",
    ]
    for e in engines:
        out.append("\t{ %2d, 0x%x, { %d, %d, %d, %d }, %s },   /* slot %2d */"
                   % ((e["plaits"], e["centre"]) + tuple(e["levels"])
                      + (c_str(e["name"]), e["slot"])))
    out += ["};", "", "#endif /* SP1_ENGINES_GEN_H */", ""]
    text = "\n".join(out)
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    try:
        with open(out_path, encoding="utf-8") as f:
            if f.read() == text:
                return                     # unchanged: don't force a recompile
    except OSError:
        pass
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


if __name__ == "__main__":
    a = sys.argv[1:]
    if a == ["--list"]:
        for idx, name, det in PLAITS_ENGINES:
            print("%-18s Plaits #%-2d detents %s" % (name, idx, det or "-"))
    elif len(a) == 2:
        CSV_NAME = os.path.normpath(a[0])
        generate(a[0], a[1])
    else:
        sys.stderr.write(__doc__)
        sys.exit(2)
