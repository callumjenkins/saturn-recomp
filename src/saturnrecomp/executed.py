"""Whether every instruction a reference run executed has a translation: a check of the map.

    python -m saturnrecomp.executed GAME.toml RUN.tsv ...

Each RUN.tsv lists what a reference (an instrumented emulator) executed, one distinct instruction a line,
TAB-separated: CPU (M, S or 68K), ADDRESS and OPCODE in hex, then anything. Lines starting with # are
comments. For the SH-2s, an address is placed by where its bytes are: work RAM addresses are folded
onto their first mirror, and a module holds the instruction when the address falls in its image and its
image has OPCODE there (two programs can load at one address; the bytes say which ran). Each is then:

    translated       a module that holds it has it in the generator's map (build/recomp/map)
    untranslated     a module holds it, but discovery never found it: code the recompile is missing
    BIOS             in the BIOS ROM, whose services the runtime replaces
    no module        no module has these bytes there: code copied, built or patched at run time,
                     or a program game.toml does not list
    elsewhere        any other address (on-chip RAM, cartridge, ...)

68000 addresses are counted, not placed. Writes BUILD/inventory/executed/summary.txt and, for every
class but translated and BIOS, a TSV of its addresses (module, first and last address of each run,
instructions in it, and the reference files that executed it).
"""
import argparse
import collections
import os
import sys

from . import config


def fold(address):
    """The address on its first mirror, for work RAM and the BIOS; others unchanged but for the cache bits."""
    a = address & 0x1FFFFFFF
    if 0x06000000 <= a < 0x08000000:
        return 0x06000000 | a & 0xFFFFF
    if 0x00200000 <= a < 0x00300000:
        return a
    if a < 0x00100000:
        return a & 0x7FFFF
    return a


def read_runs(paths):
    """{(cpu, address, opcode): [file names]} over the reference files."""
    out = collections.defaultdict(list)
    for path in paths:
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            cpu, address, opcode, *_ = line.split("\t")
            key = (cpu, int(address, 16), int(opcode, 16))
            if os.path.basename(path) not in out[key]:
                out[key].append(os.path.basename(path))
    return out


def read_mapped(path):
    """The instruction addresses the map holds."""
    return {int(line.split("\t", 1)[0], 16) for line in open(path, encoding="utf-8") if not line.startswith("#")}


def classify(executed, modules, mapped):
    """{class: [(module or "", folded address, files)]} for the SH-2 instructions in `executed`.
    `modules`: [(name, base, bytes)]; `mapped`: {name: addresses in its map}."""
    out = collections.defaultdict(list)
    for (cpu, address, opcode), files in sorted(executed.items()):
        if cpu == "68K":
            out["68K"].append(("", address, files))
            continue
        a = fold(address)
        if a < 0x00080000:
            out["BIOS"].append(("", a, files))
            continue
        holders = [name for name, base, data in modules
                   if base <= a < base + len(data) - 1 and int.from_bytes(data[a - base:a - base + 2], "big") == opcode]
        if not holders:
            inside = any(base <= a < base + len(data) for _, base, data in modules)
            ram = 0x06000000 <= a < 0x06100000 or 0x00200000 <= a < 0x00300000
            out["no module" if inside or ram else "elsewhere"].append(("", a, files))
        elif any(a in mapped[name] for name in holders):
            out["translated"].append((holders[0], a, files))
        else:
            out["untranslated"].append(("/".join(holders), a, files))
    return out


def runs(rows):
    """[(module, first, last, instructions, files)] for consecutive halfwords of one module."""
    out = []
    for module, a, files in sorted(rows, key=lambda r: (r[0], r[1])):
        if out and out[-1][0] == module and a - out[-1][2] <= 2:
            m, first, _, n, seen = out[-1]
            out[-1] = (m, first, a, n + 1, seen | set(files))
        else:
            out.append((module, a, a, 1, set(files)))
    return out


def check(game, paths, log=print):
    modules = [(m.name, m.base, open(m.file, "rb").read()) for m in game.modules]
    mapped = {}
    for m in game.modules:
        path = os.path.join(game.build, "recomp", "map", m.name + ".tsv")
        if not os.path.exists(path):
            raise SystemExit(f"no map at {path}; recompile first")
        mapped[m.name] = read_mapped(path)
    classes = classify(read_runs(paths), modules, mapped)
    out = os.path.join(game.build, "inventory", "executed")
    os.makedirs(out, exist_ok=True)
    lines = [f"Executed instructions in {len(paths)} reference runs ({', '.join(os.path.basename(p) for p in paths)}):"]
    for c in ("translated", "untranslated", "BIOS", "no module", "elsewhere", "68K"):
        rows = classes.get(c, [])
        lines.append(f"  {c:13} {len(rows):8,}")
        if c in ("translated", "BIOS") or not rows:
            continue
        with open(os.path.join(out, c.replace(" ", "-") + ".tsv"), "w", encoding="utf-8") as f:
            f.write("# module\tfirst\tlast\tinstructions\truns\n")
            for module, first, last, n, files in runs(rows):
                f.write(f"{module}\t{first:08X}\t{last:08X}\t{n}\t{','.join(sorted(files))}\n")
    per = collections.Counter(m for m, _, _ in classes.get("translated", []))
    lines.append("Translated, by module: " + ", ".join(f"{m} {n:,}" for m, n in sorted(per.items())))
    with open(os.path.join(out, "summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    log("\n".join(lines))
    return classes


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("config")
    ap.add_argument("runs", nargs="+")
    a = ap.parse_args(argv)
    classes = check(config.load(a.config), a.runs)
    sys.exit(1 if classes.get("untranslated") else 0)


if __name__ == "__main__":
    main()
