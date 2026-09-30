#!/usr/bin/env python3
"""Refuse a firmware image that could harm the one device. Run after every build.

    python wakes-sp1/tools/ci/check_image.py build [--repo wakes-sp1]

`build` is the west build directory (or its `zephyr/` subdirectory). Exits non-zero
if ANY check fails; prints every check either way, so the output is the evidence.

These checks restate docs/SAFETY.md, they do not relax it. If one fails, fix the
build -- never the check. The same script runs in CI and at the desk, so CI cannot
pass something the local loop would have caught, or the other way round.
"""
import argparse
import re
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile

APP_START = 0x20000   # slot0_partition. Below this is the TE bootloader: NEVER written.
APP_END = 0xFF000     # storage_partition: declared, never written (see the board DTS).
NAME = "wakes-sp1"    # CONFIG_KERNEL_BIN_NAME

# Constraints from docs/SAFETY.md. A build that drops any of these must not ship.
REQUIRED_CONFIG = {
    "CONFIG_USE_DT_CODE_PARTITION": "y",
    "CONFIG_FLASH_LOAD_OFFSET": "0x20000",
    "CONFIG_WATCHDOG": "y",
    "CONFIG_REBOOT": "y",
    "CONFIG_CLOCK_CONTROL_NRF_K32SRC_SYNTH": "y",
}

results = []


def check(ok, what, detail=""):
    results.append(ok)
    print(f"  [{'PASS' if ok else 'FAIL'}] {what}" + (f"  ({detail})" if detail else ""))


def zephyr_dir(p):
    p = Path(p)
    return p / "zephyr" if (p / "zephyr" / f"{NAME}.elf").exists() else p


def check_elf(zd):
    with open(zd / f"{NAME}.elf", "rb") as fh:
        elf = ELFFile(fh)
        syms = elf.get_section_by_name(".symtab").get_symbol_by_name("_vector_table") or []
        vt = [s["st_value"] for s in syms]
        check(vt == [APP_START], "_vector_table at 0x20000",
              ", ".join(hex(v) for v in vt) or "symbol missing")

        # Every byte that gets written to flash, by load address. Checking the vector
        # table alone would miss a stray section placed anywhere else.
        loads = [(s["p_paddr"], s["p_filesz"]) for s in elf.iter_segments()
                 if s["p_type"] == "PT_LOAD" and s["p_filesz"] > 0]
        lo = min(a for a, _ in loads)
        hi = max(a + n for a, n in loads)
        check(lo >= APP_START, "no load segment below 0x20000", f"lowest LMA {lo:#x}")
        check(hi <= APP_END, "no load segment at or above 0xFF000", f"image ends {hi:#x}")
        return hi - lo


def check_bin(zd, span):
    size = (zd / f"{NAME}.bin").stat().st_size
    check(size == span, f"{NAME}.bin matches the ELF's loadable span",
          f"{size} B vs {span} B")


def check_map(zd):
    text = (zd / f"{NAME}.map").read_text(encoding="utf-8", errors="replace")
    m = re.search(r"^FLASH\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)", text, re.M)
    origin, length = (int(m.group(1), 16), int(m.group(2), 16)) if m else (None, None)
    check(origin == APP_START, ".map: FLASH region origin 0x20000",
          f"{origin:#x}" if m else "no FLASH line")
    check(m is not None and origin + length == APP_END, ".map: FLASH region ends at 0xFF000",
          f"{origin + length:#x}" if m else "")
    m = re.search(r"^\s+(0x[0-9a-f]+)\s+_vector_table\s*$", text, re.M)
    check(m is not None and int(m.group(1), 16) == APP_START, ".map: _vector_table at 0x20000",
          m.group(1) if m else "not found")


def check_config(zd):
    cfg = {}
    for line in (zd / ".config").read_text(encoding="utf-8").splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            k, v = line.split("=", 1)
            cfg[k] = v.strip('"')
    for k, want in REQUIRED_CONFIG.items():
        got = cfg.get(k)
        check(got is not None and got.lower() == want, f"{k}={want}", f"got {got}")
    return cfg


def check_layouts(zd):
    """Every Plaits / Marbles / stmlib class must have ONE size across the whole image.

    The overrides change class layouts (voice.h, chord_engine.h, ...) and rely on every
    file seeing the overridden header first. If one object is compiled against upstream's
    header instead, the program holds two layouts of the same class and nothing fails
    to build: on 2026-09-30 (#22) voice.cc saw an 8516-byte plaits::Voice while
    sp1_synth.cc allocated 8584, Voice::Init wrote past the fields it thought it had,
    and the device reset before STANDBY on every boot. The compiler cannot see across
    objects and the linker does not compare layouts, so it is checked here, from DWARF.
    """
    watched = ("eurorack", "plaits_overrides", "plaits_ovr", "/src/sp1_", "\\src\\sp1_")
    sizes = {}                      # qualified name -> {byte size: [compile units]}
    with open(zd / f"{NAME}.elf", "rb") as fh:
        elf = ELFFile(fh)
        if not elf.has_dwarf_info():
            check(False, "one layout per Plaits/Marbles class", "no DWARF in the ELF")
            return
        for cu in elf.get_dwarf_info().iter_CUs():
            top = cu.get_top_DIE()
            cu_name = top.attributes["DW_AT_name"].value.decode(errors="replace") \
                if "DW_AT_name" in top.attributes else "?"
            if not any(w in cu_name for w in watched):
                continue

            def walk(die, scope):
                for child in die.iter_children():
                    tag = child.tag
                    name = child.attributes.get("DW_AT_name")
                    name = name.value.decode(errors="replace") if name else None
                    if tag == "DW_TAG_namespace":
                        if name is not None:            # anonymous namespaces are per file
                            walk(child, scope + [name])
                    elif tag in ("DW_TAG_class_type", "DW_TAG_structure_type"):
                        if name is None or "DW_AT_declaration" in child.attributes:
                            continue
                        if scope and scope[0] in ("plaits", "marbles", "stmlib"):
                            size = child.attributes.get("DW_AT_byte_size")
                            if size is not None:
                                q = "::".join(scope + [name])
                                sizes.setdefault(q, {}).setdefault(size.value, []).append(cu_name)
                        walk(child, scope + [name])

            walk(top, [])

    bad = {q: s for q, s in sizes.items() if len(s) > 1}
    detail = f"{len(sizes)} classes"
    if bad:
        worst = sorted(bad.items())[:4]
        detail = "; ".join(
            f"{q}: " + ", ".join(f"{b} B in {Path(c[0]).name}" for b, c in s.items())
            for q, s in worst)
    check(not bad and len(sizes) > 0, "one layout per Plaits/Marbles class", detail)


def check_source(repo):
    # A header constant, not a build flag, so nothing else would catch a leftover 1:
    # the device would boot into the calibration wizard.
    hdr = (Path(repo) / "firmware" / "src" / "sp1_calib.h").read_text(encoding="utf-8")
    m = re.search(r"^#define\s+SP1_FORCE_CALIB\s+(\d+)", hdr, re.M)
    check(m is not None and m.group(1) == "0", "SP1_FORCE_CALIB is 0",
          f"got {m.group(1)}" if m else "define not found")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("build", help="west build directory")
    ap.add_argument("--repo", default=Path(__file__).resolve().parents[2],
                    help="wakes-sp1 repo root (default: this script's repo)")
    a = ap.parse_args()
    zd = zephyr_dir(a.build)

    print(f"check_image: {zd}")
    span = check_elf(zd)
    check_bin(zd, span)
    check_map(zd)
    cfg = check_config(zd)
    if cfg.get("CONFIG_SP1_PLAITS") == "y":
        check_layouts(zd)
    check_source(a.repo)

    failed = results.count(False)
    plaits = "plaits" if cfg.get("CONFIG_SP1_PLAITS") == "y" else "fallback"
    print(f"{NAME}.bin: {span} B ({span / (APP_END - APP_START):.2%} of the slot), {plaits} config")
    print("OK: safe to hand over for flashing" if not failed
          else f"FAILED: {failed} check(s). Do not flash this image.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
