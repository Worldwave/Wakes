#!/usr/bin/env python3
"""wakes-sp1 -- generate the firmware's engine table from config/engines.csv.

    gen_engines.py <engines.csv> <out.h>     build step (CMake runs this)
    gen_engines.py --list                    print every engine a config may name

Two inputs, deliberately kept apart:

  config/engines.csv   the USER's choice: which engine sits in which slot. One row per
                       slot, at most 24, in slot order -- ROW POSITION is the slot.
                       Columns Slot, Glyph, Engine. Only Engine is the user's: Slot and
                       Glyph are printed there so the file reads as the device does, and
                       the build checks they are unchanged. An empty Engine is an empty
                       slot, which T2/T3 engine select skips. Any column after the third
                       is ignored: the shipped file uses a fourth, "Engines you can choose
                       from", as a reference list (a CSV has no comments).
  this file            FACTS the user cannot choose: each slot's glyph (SLOT_GLYPHS) and
                       each engine's index inside Plaits and centre detents
                       (PLAITS_ENGINES, keyed by name).

Glyphs are four symbols, T1 first: U+25CF full, U+25D0 half, U+25CB off. The CSV may
also spell them # + . -- editors and spreadsheets sometimes mangle the circles.

Standard library only. The CSV is read as UTF-8 (with or without a BOM, which Excel
writes): Windows' default code page would mangle the glyphs.
"""
import csv
import os
import sys

# ---- facts: NOT user configuration -------------------------------------------------------

# The glyph each slot flashes when it is selected, T1 first. A glyph stands for the SLOT,
# not the engine in it: drawn by hand to be recognisable (Adara, M3c), full-brightness
# for slots 1-16 and half-brightness from 17. Every glyph is different.
SLOT_GLYPHS = [
    "●○○○", "●●○○", "●●●○", "●●●●", "○●●●", "○○●●", "○○○●", "○●○○",      # 1-8
    "○○●○", "●○○●", "○●●○", "●○●●", "●●○●", "●○●○", "○●○●", "○○○○",      # 9-16
    "◐◐◐◐", "○◐◐○", "◐○○◐", "○◐○◐", "◐○◐○", "○◐◐◐", "○○○◐", "○◐○○",      # 17-24
]
MAX_SLOTS = len(SLOT_GLYPHS)

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

# -------------------------------------------------------------------------------------------

GLYPH_LEVEL = {"○": 0, ".": 0, "◐": 1, "+": 1, "●": 2, "#": 2}
ASCII_GLYPH = {ord("●"): "#", ord("◐"): "+", ord("○"): "."}
COLUMNS = ("slot", "glyph", "engine")
CSV_NAME = "config/engines.csv"


def fail(msg):
    msg = "\n*** %s: %s\n\n" % (CSV_NAME, msg)
    try:
        msg.encode(sys.stderr.encoding or "ascii")
    except UnicodeEncodeError:
        # A Windows console (cp1252) cannot show the circles; the ASCII forms can.
        msg = msg.translate(ASCII_GLYPH)
    sys.stderr.write(msg)
    sys.exit(1)


def levels(glyph):
    """Four symbols -> [0|1|2] * 4, or None if it is not a glyph."""
    syms = [ch for ch in glyph if not ch.isspace()]
    if len(syms) != 4 or any(ch not in GLYPH_LEVEL for ch in syms):
        return None
    return [GLYPH_LEVEL[ch] for ch in syms]


def facts():
    assert len({tuple(levels(g)) for g in SLOT_GLYPHS}) == MAX_SLOTS, "slot glyphs must differ"
    by_name = {}
    for idx, name, det in PLAITS_ENGINES:
        bits = 0
        for d in det.split():
            bits |= DETENT_BITS[d]
        by_name[name.lower()] = dict(plaits=idx, name=name, centre=bits)
    assert sorted(e["plaits"] for e in by_name.values()) == list(range(24))
    return by_name


def parse(csv_path):
    known = facts()
    with open(csv_path, encoding="utf-8-sig", newline="") as f:
        rows = [(n + 1, r) for n, r in enumerate(csv.reader(f)) if r]
    if not rows:
        fail("the file is empty")
    header = [c.strip().lower() for c in rows[0][1]]
    if tuple(header[:3]) != COLUMNS:
        fail("line %d: the first row must be the header Slot,Glyph,Engine -- got %s"
             % (rows[0][0], ",".join(rows[0][1])))
    rows = rows[1:]
    if len(rows) > MAX_SLOTS:
        fail("%d slots listed; there are at most %d" % (len(rows), MAX_SLOTS))

    slots, used = [], {}
    for n, (line, r) in enumerate(rows):
        slot = n + 1
        r = [c.strip() for c in r] + [""] * 3
        slot_t, glyph_t, name_t = r[:3]
        where = "line %d (slot %d)" % (line, slot)
        # Slot and Glyph are labels, not settings: row position is the slot, and the
        # glyph belongs to the slot. A mismatch means a whole row was moved, or edited.
        if slot_t != str(slot):
            fail("%s: the Slot column says %r, but this is row %d, so slot %d.\n"
                 "    Row position is the slot and cannot be changed. To reorder, move the\n"
                 "    Engine names between rows, not whole rows." % (where, slot_t, slot, slot))
        want = SLOT_GLYPHS[n]
        got = levels(glyph_t)
        if got != levels(want):
            hint = ("" if got is not None else
                    "\n    If the circles came out as '?' or garbage, your editor saved in the"
                    "\n    wrong encoding: save as UTF-8, or write the glyph as `%s`."
                    % want.translate(ASCII_GLYPH))
            fail("%s: slot %d's glyph is %s and cannot be changed -- the file says `%s`.%s"
                 % (where, slot, want, glyph_t, hint))
        entry = dict(slot=slot, levels=levels(want), on=False, plaits=0, centre=0, name="")
        if name_t:
            e = known.get(name_t.lower())
            if e is None:
                fail("%s: no engine is called %r. Available engines:\n    %s"
                     % (where, name_t, "\n    ".join(n for _, n, _ in PLAITS_ENGINES)))
            if e["name"] in used:
                fail("%s: %s is already in slot %d; an engine can be listed once"
                     % (where, e["name"], used[e["name"]]))
            used[e["name"]] = slot
            entry.update(e, on=True)
        slots.append(entry)

    if not used:
        fail("every slot is empty; the firmware needs at least one engine")
    return slots


def c_str(s):
    s = s.encode("ascii", "replace").decode("ascii")[:24]
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def generate(csv_path, out_path):
    slots = parse(csv_path)
    # The device starts on the first slot with an engine in it (M4, Adara: slot 1 by default).
    default = next(n for n, s in enumerate(slots) if s["on"])
    out = [
        "/* GENERATED from config/engines.csv by tools/gen_engines.py.",
        " * Do not edit: edit the CSV and rebuild. */",
        "#ifndef SP1_ENGINES_GEN_H",
        "#define SP1_ENGINES_GEN_H",
        "",
        "#include <stdint.h>",
        "",
        "#define SP1_ENGINE_SLOTS        %d" % len(slots),
        "#define SP1_ENGINE_DEFAULT_SLOT %d   /* the first slot with an engine */" % default,
        "",
        "/* on:     0 = an empty slot, which engine select skips",
        " * centre: 0x1 = F4 HARMONICS, 0x2 = F2 TIMBRE, 0x4 = F3 MORPH",
        " * led:    T1..T4, 0 = off, 1 = half, 2 = full (the slot's fixed glyph) */",
        "static const struct {",
        "\tuint8_t plaits;     /* index in plaits/dsp/voice.cc */",
        "\tuint8_t on;",
        "\tuint8_t centre;",
        "\tuint8_t led[4];",
        "\tconst char *name;",
        "} SP1_ENGINE_TABLE[SP1_ENGINE_SLOTS] = {",
    ]
    for s in slots:
        out.append("\t{ %2d, %d, 0x%x, { %d, %d, %d, %d }, %s },   /* slot %2d */"
                   % ((s["plaits"], int(s["on"]), s["centre"]) + tuple(s["levels"])
                      + (c_str(s["name"]), s["slot"])))
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
