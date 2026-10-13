"""Where each original instruction ended up in the executable: native address ranges for the map.

    python -m saturnrecomp.native GAME.toml

Builds the generated code again in BUILD/recomp-lines with -gline-tables-only, which in clang leaves the
code itself alone, and stops unless that executable's .text is byte for byte the built one's: then the
line tables describe the executable that runs. Reads them with readelf and writes, into
BUILD/inventory/native:

    <MODULE>.tsv  each row of the generator's map (build/recomp/map) with the native ranges of its line,
                  "START+LENGTH,..." in hex, or nothing when the line has no code of its own
    summary.txt   per module, the instructions (counted once per address) with native code, and those
                  with none: merged into a neighbouring line's code, or optimised away

A line's ranges run from each address the line table starts it at to the next row for a generated
line, so code the compiler inlined from the runtime's headers counts towards the line that called it.
"""
import argparse
import collections
import hashlib
import os
import re
import subprocess

from . import config

GENERATED = re.compile(r"p_[a-z0-9_]+_\d+\.cpp$")


def line_build(game, log=print):
    """The executable built with line tables, in BUILD/recomp-lines."""
    out = os.path.join(game.build, "recomp-lines")
    args = ["cmake", "-S", os.path.join(game.build, "recomp"), "-B", out, "-G", "Ninja",
            "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_CXX_FLAGS=-gline-tables-only", *(f"-D{o}" for o in game.cmake)]
    for step in (args, ["ninja", "-C", out, "saturn"]):
        r = subprocess.run(step, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"{' '.join(step[:2])} failed:\n{(r.stdout + r.stderr)[-3000:]}")
    log(f"built {out}")
    return os.path.join(out, os.path.basename(game.saturn))


def text_sha1(executable):
    r = subprocess.run(["objcopy", "-O", "binary", "-j", ".text", executable, "/dev/stdout"], capture_output=True)
    if r.returncode:
        raise SystemExit(f"objcopy failed on {executable}: {r.stderr.decode()[-500:]}")
    return hashlib.sha1(r.stdout).hexdigest()


def line_ranges(executable):
    """{(file name, line): [(start, length)]} for the generated files, from the decoded line table. A
    generated line runs to the next row for a generated file or the end of its sequence; rows for the
    runtime's headers in between are code inlined into it. Line 0 (code with no line) goes to nothing."""
    proc = subprocess.Popen(["readelf", "--debug-dump=decodedline", "-W", executable],
                            stdout=subprocess.PIPE, text=True, bufsize=1 << 20)
    out = parse(proc.stdout)
    proc.wait()
    return out


def parse(rows):
    """line_ranges' work on readelf's decoded rows."""
    out = collections.defaultdict(list)
    current = None                                  # (file, line, start) of the generated line being read
    for text in rows:
        parts = text.split()
        if len(parts) < 3 or not parts[2].startswith("0x"):
            continue
        name, line, address = parts[0], parts[1], int(parts[2], 16)
        generated = GENERATED.search(name)
        if line == "-" or generated:
            if current and address > current[2]:
                out[current[:2]].append((current[2], address - current[2]))
            current = None
        if generated and line not in ("-", "0"):
            current = (os.path.basename(name), int(line), address)
    return out


def merge(ranges):
    out = []
    for start, n in sorted(ranges):
        if out and start <= out[-1][0] + out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], start + n - out[-1][0]))
        else:
            out.append((start, n))
    return out


def native(game, log=print):
    exe = line_build(game, log)
    if not os.path.exists(game.saturn):
        raise SystemExit(f"no build at {game.saturn}")
    if text_sha1(exe) != text_sha1(game.saturn):
        raise SystemExit(f"{exe}'s code differs from {game.saturn}'s: its line tables would not describe the "
                         "build. Rebuild (python -m saturnrecomp.build) and run this again.")
    ranges = line_ranges(exe)
    out = os.path.join(game.build, "inventory", "native")
    os.makedirs(out, exist_ok=True)
    lines = ["Instructions with native code, and with none (merged into a neighbouring line, or optimised away):"]
    for m in game.modules:
        path = os.path.join(game.build, "recomp", "map", m.name + ".tsv")
        if not os.path.exists(path):
            raise SystemExit(f"no map at {path}; recompile first")
        placed, unplaced = set(), set()
        with open(path, encoding="utf-8") as f, open(os.path.join(out, m.name + ".tsv"), "w", encoding="utf-8") as w:
            w.write("# address\tfunction\tkind\tfile\tline\tnative\n")
            for row in f:
                if row.startswith("#"):
                    continue
                a, func, kind, name, n = row.rstrip("\n").split("\t")
                where = merge(ranges.get((name, int(n)), []))
                (placed if where else unplaced).add(a)
                w.write(f"{a}\t{func}\t{kind}\t{name}\t{n}\t{','.join(f'{s:x}+{k:x}' for s, k in where)}\n")
        unplaced -= placed
        lines.append(f"  {m.name:6} {len(placed):7,} placed, {len(unplaced):6,} with none")
    lines.append(f"Line tables from {exe}, whose .text equals {game.saturn}'s.")
    with open(os.path.join(out, "summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    log("\n".join(lines))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("config")
    a = ap.parse_args(argv)
    native(config.load(a.config))


if __name__ == "__main__":
    main()
