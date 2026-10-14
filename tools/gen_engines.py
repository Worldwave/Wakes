#!/usr/bin/env python3
"""wakes-sp1 -- generate the firmware's engine table from config/engines.cnfg.

    gen_engines.py <engines.cnfg> <out.h>    build step (CMake runs this)
    gen_engines.py --list                    print every engine a config may name

Two inputs, deliberately kept apart:

  config/engines.cnfg  the DEFAULT engine stack (ini, #52): `slot = engine` under [engines],
                       slots 1-24. The file is the whole stack: a slot it does not list, or
                       lists with no name (or `none`), is empty, and T2/T3 skip it. Each
                       line may carry the slot's glyph as a comment; the build checks it.
  this file            FACTS the user cannot choose: each slot's glyph (SLOT_GLYPHS) and
                       each engine's index inside Plaits and centre detents
                       (PLAITS_ENGINES, keyed by name).

Glyphs are four symbols, T1 first: U+25CF full, U+25D0 half, U+25CB off (or # + .).

The build fails, naming the line, on anything wrong in the default: the repository's copy
must be exactly right. (The device's own reader of the drive's copy, when it comes, keeps
going instead -- sp1_cnfg.h's rules.)

Standard library only. Read as UTF-8, with or without a BOM.
"""
import os
import re
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
    (0,  "VA-VCF",           "F3 F4"),
    (1,  "phase-distortion", ""),
    (2,  "6-op-FM-A",        "F3"),
    (3,  "6-op-FM-B",        "F3"),
    (4,  "6-op-FM-C",        "F3"),
    (5,  "wave-terrain",     "F3"),
    (6,  "string-machine",   "F2"),
    (7,  "chiptune",         ""),
    (8,  "virtual-analog",   "F2 F4"),
    (9,  "waveshaping",      "F4"),
    (10, "2-op-FM",          "F3"),
    (11, "grain-formant",    "F4"),
    (12, "additive",         ""),
    (13, "wavetable",        ""),
    (14, "chords",           ""),
    (15, "speech",           ""),
    (16, "swarm",            ""),
    (17, "filtered-noise",   "F4"),
    (18, "particle",         "F3"),
    (19, "string",           ""),
    (20, "modal",            ""),
    (21, "bass-drum",        ""),
    (22, "snare-drum",       ""),
    (23, "hi-hat",           ""),
]
DETENT_BITS = {"F4": 0x1, "F2": 0x2, "F3": 0x4}   # HARMONICS, TIMBRE, MORPH

# -------------------------------------------------------------------------------------------

GLYPH_LEVEL = {"○": 0, ".": 0, "◐": 1, "+": 1, "●": 2, "#": 2}
ASCII_GLYPH = {ord("●"): "#", ord("◐"): "+", ord("○"): "."}
CNFG_NAME = "config/engines.cnfg"
LINE_MAX = 160                       # SP1_CNFG_LINE_MAX: the device ignores longer lines


def fail(msg):
    msg = "\n*** %s: %s\n\n" % (CNFG_NAME, msg)
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


def name_key(name):
    """How names are compared, here and on the device (sp1_cnfg.h, sp1_engine_name_eq):
    letters and digits only, case ignored -- so "grain / formant", "grain-formant" and
    "Grain Formant" are one engine, and a PRST file saved under an older spelling loads."""
    return "".join(ch for ch in name.lower() if ch.isascii() and ch.isalnum())


def facts():
    assert len({tuple(levels(g)) for g in SLOT_GLYPHS}) == MAX_SLOTS, "slot glyphs must differ"
    by_name = {}
    for idx, name, det in PLAITS_ENGINES:
        bits = 0
        for d in det.split():
            bits |= DETENT_BITS[d]
        by_name[name_key(name)] = dict(plaits=idx, name=name, centre=bits)
    assert sorted(e["plaits"] for e in by_name.values()) == list(range(24)), "names must differ"
    return by_name


def ini_lines(path):
    """(line number, key, value, comment) for every setting, split the way the device's
    reader splits them (sp1_cnfg.c): ; or # at the value's start or after whitespace starts
    a comment."""
    with open(path, encoding="utf-8-sig") as f:
        lines = f.read().split("\n")
    if not lines[0].lstrip().startswith(";"):
        fail('line 1: the file must start with a comment naming its format ("; Format: ini")')
    section, out = "", []
    for n, raw in enumerate(lines, 1):
        if len(raw.rstrip("\r").encode("utf-8")) > LINE_MAX:
            fail("line %d is over %d bytes: the device would ignore it" % (n, LINE_MAX))
        s = raw.strip()
        if not s or s[0] in ";#":
            continue
        if s[0] == "[":
            if not s.endswith("]"):
                fail("line %d: a section name needs a closing ]" % n)
            section = s[1:-1].strip().lower()
            if section != "engines":
                fail("line %d: unknown section [%s] (expected [engines])" % (n, section))
            continue
        if not section:
            fail("line %d: %r is not under [engines]" % (n, s))
        if "=" not in s:
            fail("line %d: %r is not a setting (slot = engine)" % (n, s))
        key, val = s.split("=", 1)
        m = re.search(r"(^|\s)[;#]", val)
        comment = val[m.end():].strip() if m else ""
        val = (val[:m.start()] if m else val).strip()
        out.append((n, key.strip().lower(), val, comment))
    return out


def parse(path):
    known = facts()
    slots = [dict(slot=n + 1, levels=levels(g), on=False, plaits=0, centre=0, name="")
             for n, g in enumerate(SLOT_GLYPHS)]
    seen, used = {}, {}
    for line, key, val, comment in ini_lines(path):
        where = "line %d" % line
        if not key.isdigit() or not 1 <= int(key) <= MAX_SLOTS:
            fail("%s: %r is not a slot (1-%d)" % (where, key, MAX_SLOTS))
        slot = int(key)
        if slot in seen:
            fail("%s: slot %d is already set on line %d" % (where, slot, seen[slot]))
        seen[slot] = line
        want = SLOT_GLYPHS[slot - 1]
        if comment and levels(comment) is not None and levels(comment) != levels(want):
            fail("%s: slot %d's glyph is %s; the comment says %s. The glyph belongs to the\n"
                 "    slot: move engine NAMES between slots, not whole lines with their glyphs."
                 % (where, slot, want, comment))
        if not val or val.lower() == "none":
            continue
        e = known.get(name_key(val))
        if e is None:
            fail("%s: no engine is called %r. Available engines:\n    %s"
                 % (where, val, "\n    ".join(n for _, n, _ in PLAITS_ENGINES)))
        if e["name"] in used:
            fail("%s: %s is already in slot %d; an engine can be listed once"
                 % (where, e["name"], used[e["name"]]))
        used[e["name"]] = slot
        slots[slot - 1].update(e, on=True)
    if not used:
        fail("every slot is empty; the firmware needs at least one engine")
    return slots


def c_str(s):
    s = s.encode("ascii", "replace").decode("ascii")[:24]
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def generate(path, out_path):
    slots = parse(path)
    # The device starts on the first slot with an engine in it (M4, Adara: slot 1 by default).
    default = next(n for n, s in enumerate(slots) if s["on"])
    out = [
        "/* GENERATED from config/engines.cnfg by tools/gen_engines.py.",
        " * Do not edit: edit config/engines.cnfg and rebuild. */",
        "#ifndef SP1_ENGINES_GEN_H",
        "#define SP1_ENGINES_GEN_H",
        "",
        "#include <stdint.h>",
        "",
        "#define SP1_ENGINE_SLOTS        %d" % len(slots),
        "#define SP1_ENGINE_DEFAULT_SLOT %d   /* the first slot with an engine */" % default,
        "",
        "/* The built-in stack as Plaits indices, 255 = an empty slot: the initialiser of the",
        " * RAM stack that WAKES/CNFG/engines.cnfg replaces at ON (sp1_plaits_ui.c, #52). */",
        "#define SP1_ENGINE_EMPTY 255u",
        "#define SP1_ENGINE_DEFAULT_STACK { %s }" % ", ".join(
            str(s["plaits"]) if s["on"] else "255" for s in slots),
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
    # Every engine in the build, listed or not, by its Plaits index (#50): a PRST slot may
    # name an engine that is not in the stack, and the firmware plays it -- every build
    # contains all of Plaits' engines; engines.cnfg only chooses which ones T2/T3 reach.
    by_idx = sorted(facts().values(), key=lambda e: e["plaits"])
    out += ["};", "",
            "#define SP1_PLAITS_ENGINE_COUNT %d" % len(by_idx), "",
            "/* By Plaits index: name and centre detents (as above), listed or not. */",
            "static const struct {",
            "\tuint8_t centre;",
            "\tconst char *name;",
            "} SP1_PLAITS_ENGINES[SP1_PLAITS_ENGINE_COUNT] = {"]
    for e in by_idx:
        out.append("\t{ 0x%x, %s },   /* plaits %2d */" % (e["centre"], c_str(e["name"]), e["plaits"]))
    out += ["};", ""]
    # #52: the file itself, for wakes-sp1-fresh to write to WAKES/CNFG/engines.cnfg.
    with open(path, encoding="utf-8-sig") as f:
        template = f.read().replace("\r\n", "\n")
    out += ["#if defined(SP1_CNFG_TEMPLATES)",
            "static const char SP1_ENGINES_TEMPLATE[] ="]
    for ln in template.splitlines(keepends=True):
        lit = ""
        for b in ln.encode("utf-8"):
            ch = chr(b)
            if ch == "\\":
                lit += "\\\\"
            elif ch == '"':
                lit += '\\"'
            elif ch == "\n":
                lit += "\\n"
            elif ch == "\t":
                lit += "\\t"
            elif 0x20 <= b < 0x7F:
                lit += ch
            else:
                lit += "\\%03o" % b
        out.append('\t"%s"' % lit)
    out += ["\t;" if template else '\t"";', "#endif", "", "#endif /* SP1_ENGINES_GEN_H */", ""]
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
        CNFG_NAME = os.path.normpath(a[0])
        generate(a[0], a[1])
    else:
        sys.stderr.write(__doc__)
        sys.exit(2)
