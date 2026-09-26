#!/usr/bin/env python3
"""Firmware size, and the difference between two builds.

    python wakes-sp1/tools/ci/size_report.py report build -o head.json
    python wakes-sp1/tools/ci/size_report.py diff base.json head.json [--title "..."]
    python wakes-sp1/tools/ci/size_report.py show head.json [--title "..."]

`report` reads the ELF and .config of one build. `diff` prints a Markdown table
(for a CI job summary, a release body, or the terminal); `show` is the same table
for one build, when there is nothing to compare against.

Growth is information, not a failure: a new subsystem legitimately moves the binary
(USB CDC was ~20-40 KB, Marbles ~99 KB). The linker already fails an image that
outgrows the slot, and check_image.py owns every safety question.
"""
import argparse
import json
import sys
from pathlib import Path

from elftools.elf.constants import SH_FLAGS
from elftools.elf.elffile import ELFFile

NAME = "wakes-sp1"
FLASH = (0x20000, 0xFF000)          # slot0_partition
RAM = (0x20000000, 0x20040000)      # 256 KB


def zephyr_dir(p):
    p = Path(p)
    return p / "zephyr" if (p / "zephyr" / f"{NAME}.elf").exists() else p


def report(build):
    zd = zephyr_dir(build)
    sections = {}
    ram = 0
    with open(zd / f"{NAME}.elf", "rb") as fh:
        elf = ELFFile(fh)
        for s in elf.iter_sections():
            if not s["sh_flags"] & SH_FLAGS.SHF_ALLOC or not s["sh_size"]:
                continue
            sections[s.name] = s["sh_size"]
            if RAM[0] <= s["sh_addr"] < RAM[1]:
                ram += s["sh_size"]
    cfg = (zd / ".config").read_text(encoding="utf-8")
    return {
        "config": "plaits" if "\nCONFIG_SP1_PLAITS=y" in cfg else "fallback",
        "bin": (zd / f"{NAME}.bin").stat().st_size,
        "ram": ram,
        "sections": sections,
    }


def delta(a, b):
    d = b - a
    return "0" if d == 0 else f"{d:+,}"


def diff(base, head, title):
    out = [f"### {title}" if title else "", "",
           f"Config: **{head['config']}**", "",
           "| | base | head | Δ | of total |",
           "|---|---:|---:|---:|---:|"]
    for key, label, total in (("bin", "FLASH (`.bin`)", FLASH[1] - FLASH[0]),
                              ("ram", "RAM", RAM[1] - RAM[0])):
        out.append(f"| {label} | {base[key]:,} | {head[key]:,} | "
                   f"{delta(base[key], head[key])} | {head[key] / total:.2%} |")

    # The sections that moved, largest change first -- that is where to look.
    names = set(base["sections"]) | set(head["sections"])
    moved = sorted(((head["sections"].get(n, 0) - base["sections"].get(n, 0), n)
                    for n in names), key=lambda t: -abs(t[0]))
    moved = [(d, n) for d, n in moved if d][:12]
    if moved:
        out += ["", "<details><summary>Sections that changed</summary>", "",
                "| section | base | head | Δ |", "|---|---:|---:|---:|"]
        for d, n in moved:
            out.append(f"| `{n}` | {base['sections'].get(n, 0):,} | "
                       f"{head['sections'].get(n, 0):,} | {d:+,} |")
        out += ["", "</details>"]
    return "\n".join(out).lstrip() + "\n"


def show(head, title):
    out = [f"### {title}" if title else "", "",
           f"Config: **{head['config']}**", "",
           "| | size | of total |", "|---|---:|---:|"]
    for key, label, total in (("bin", "FLASH (`.bin`)", FLASH[1] - FLASH[0]),
                              ("ram", "RAM", RAM[1] - RAM[0])):
        out.append(f"| {label} | {head[key]:,} | {head[key] / total:.2%} |")
    return "\n".join(out).lstrip() + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("report")
    r.add_argument("build")
    r.add_argument("-o", "--output")
    d = sub.add_parser("diff")
    d.add_argument("base")
    d.add_argument("head")
    d.add_argument("--title", default="")
    s = sub.add_parser("show")
    s.add_argument("head")
    s.add_argument("--title", default="")
    a = ap.parse_args()
    # The table has a Δ in it; a Windows console defaults to cp1252.
    sys.stdout.reconfigure(encoding="utf-8")

    if a.cmd == "report":
        text = json.dumps(report(a.build), indent=1, sort_keys=True)
        if a.output:
            Path(a.output).write_text(text + "\n", encoding="utf-8")
        else:
            print(text)
    else:
        load = lambda p: json.loads(Path(p).read_text(encoding="utf-8"))
        sys.stdout.write(diff(load(a.base), load(a.head), a.title) if a.cmd == "diff"
                         else show(load(a.head), a.title))
    return 0


if __name__ == "__main__":
    sys.exit(main())
