"""Whether every instruction a reference run executed has a translation: a check of the map.

    python -m saturnrecomp.executed GAME.toml RUN.tsv ... [--notes NOTES.tsv]

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

A 68000 address is folded onto sound RAM's 512 KB and placed the same way among the programs game.toml's
[[inventory.sound]] lists (saturnrecomp.m68k), which load at 0:

    68K interpreted  a program that holds it has it among the instructions its descent finds
    68K undiscovered a program holds it, but the descent never found it: its region report is wrong,
                     or other code with the same bytes there ran

Either 68K class can hold code from elsewhere whose bytes happen to match a program's.
    68K no image     no program has these bytes there: the BIOS's own sound program, or code written
                     at run time

A notes file explains "no module" and "68K no image" addresses: TAB-separated FIRST, LAST (hex, folded
addresses, inclusive) and a note with the evidence. The two classes cannot meet in one row: an SH-2
address below 0x00080000 is the BIOS's, and every 68000 address is below it. The summary splits each
into explained, by note, and unexplained. Writes BUILD/inventory/executed/summary.txt and, for every
class but the translated, interpreted and BIOS ones, a TSV of its addresses (module, first and last address
of each run, instructions in it, and the reference files that executed it).
"""
import argparse
import collections
import os
import sys

from . import config, m68k


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


def names(paths):
    """A short name for each reference file: its file name, or its directory's where file names repeat."""
    base = [os.path.basename(p) for p in paths]
    return [os.path.basename(os.path.dirname(os.path.abspath(p))) if base.count(b) > 1 else b for p, b in zip(paths, base)]


def read_runs(paths):
    """{(cpu, address, opcode): [run names]} over the reference files."""
    out = collections.defaultdict(list)
    for path, name in zip(paths, names(paths)):
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            cpu, address, opcode, *_ = line.split("\t")
            key = (cpu, int(address, 16), int(opcode, 16))
            if name not in out[key]:
                out[key].append(name)
    return out


def read_mapped(path):
    """The instruction addresses the map holds."""
    return {int(line.split("\t", 1)[0], 16) for line in open(path, encoding="utf-8") if not line.startswith("#")}


def classify(executed, modules, mapped, sound=()):
    """{class: [(module or "", folded address, files)]} for the instructions in `executed`.
    `modules`: [(name, base, bytes)]; `mapped`: {name: addresses in its map}; `sound`: [(name, m68k.Program)]."""
    out = collections.defaultdict(list)
    for (cpu, address, opcode), files in sorted(executed.items()):
        if cpu == "68K":
            a = address & 0x7FFFF
            holders = [(name, p) for name, p in sound if a + 2 <= len(p.img) and p.img.u16(a) == opcode]
            if not holders:
                out["68K no image"].append(("", a, files))
            elif found := [name for name, p in holders if a in p.code]:
                out["68K interpreted"].append((found[0], a, files))
            else:
                out["68K undiscovered"].append(("/".join(name for name, _ in holders), a, files))
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


def read_notes(path):
    """[(first, last, note)]"""
    out = []
    for line in open(path, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        first, last, note = line.rstrip("\n").split("\t", 2)
        out.append((int(first, 16), int(last, 16), note))
    return out


def explain(rows, notes):
    """({note: instructions}, the rows no note covers)."""
    by, rest = collections.Counter(), []
    for row in rows:
        note = next((n for first, last, n in notes if first <= row[1] <= last), None)
        if note is None:
            rest.append(row)
        else:
            by[note] += 1
    return by, rest


NOTED = ("no module", "68K no image")


def failed(classes):
    return any(classes.get(c) for c in ("untranslated", "68K undiscovered", *NOTED))


def check(game, paths, notes=(), log=print):
    modules = [(m.name, m.base, open(m.file, "rb").read()) for m in game.modules]
    mapped = {}
    for m in game.modules:
        path = os.path.join(game.build, "recomp", "map", m.name + ".tsv")
        if not os.path.exists(path):
            raise SystemExit(f"no map at {path}; recompile first")
        mapped[m.name] = read_mapped(path)
    sound = []
    if game.inventory and game.inventory.sound:
        lib = m68k.library(os.path.join(game.build, "m68kdasm"))
        sound = [(s.name, m68k.Program(m68k.Image(s.read(game.inventory.files), lib))) for s in game.inventory.sound]
    classes = classify(read_runs(paths), modules, mapped, sound)
    out = os.path.join(game.build, "inventory", "executed")
    os.makedirs(out, exist_ok=True)
    shown = names(paths)
    lines = [f"Executed instructions in {len(paths)} reference runs ({', '.join(shown) if len(shown) <= 10 else 'listed in runs.txt'}):"]
    with open(os.path.join(out, "runs.txt"), "w", encoding="utf-8") as f:
        f.write("".join(f"{n}\t{p}\n" for n, p in zip(shown, paths)))
    explained = {}
    for c in NOTED:
        explained[c], classes[c] = explain(classes.get(c, []), notes)
    for c in ("translated", "untranslated", "BIOS", "no module", "elsewhere",
              "68K interpreted", "68K undiscovered", "68K no image"):
        rows = classes.get(c, [])
        lines.append(f"  {c:16} {len(rows):8,}" + (" unexplained" if c in NOTED else ""))
        if c in NOTED:
            lines += [f"  {'':16} {n:8,} {note}" for note, n in explained[c].most_common()]
        if c in ("translated", "BIOS", "68K interpreted") or not rows:
            continue
        with open(os.path.join(out, c.replace(" ", "-") + ".tsv"), "w", encoding="utf-8") as f:
            f.write("# module\tfirst\tlast\tinstructions\truns\n")
            for module, first, last, n, files in runs(rows):
                f.write(f"{module}\t{first:08X}\t{last:08X}\t{n}\t{','.join(sorted(files))}\n")
    per = collections.Counter(m for m, _, _ in classes.get("translated", []))
    lines.append("Translated, by module: " + ", ".join(f"{m} {n:,}" for m, n in sorted(per.items())))
    per = collections.Counter(m for m, _, _ in classes.get("68K interpreted", []))
    if per:
        lines.append("Interpreted, by 68000 program: " + ", ".join(f"{m} {n:,}" for m, n in sorted(per.items())))
    with open(os.path.join(out, "summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    log("\n".join(lines))
    return classes


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("config")
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--notes", help="explanations of addresses no module holds")
    a = ap.parse_args(argv)
    classes = check(config.load(a.config), a.runs, read_notes(a.notes) if a.notes else ())
    sys.exit(1 if failed(classes) else 0)


if __name__ == "__main__":
    main()
