"""What a game's disc holds, and what became of each part.

    python -m saturnrecomp.inventory GAME.toml

Reads game.toml's [inventory] and the map the last recompile wrote (BUILD/recomp/map). Writes into
BUILD/inventory:

    files.tsv     every file and audio track on the disc, and the system area (IP.BIN): its size,
                  SHA-1 and treatment. A file a module is made from is "translated", naming the
                  modules; any other takes the first row of the treatments file that matches it, or
                  is "unknown".
    images.tsv    each module: its file's SHA-1, the disc file it comes from, the byte ranges
                  preparation changed or added, and its halfwords by class: instructions, those of them with
                  no generated line, data and unclassified.
    gaps.tsv      each unclassified run: module, guest address, file offset, length.
    summary.txt   the totals, and each check that failed.

The treatments file is TAB-separated: PATTERN, TREATMENT, then a note giving the evidence. A pattern
is a disc path glob ("/BOMSS/*.CPK"), "track NN" for an audio track or "IP.BIN" for the system area.
A treatment is one of TREATMENTS. Lines starting with # are comments.

Instructions are counted once per address however many functions hold them, delay slots included.
A module whose image no disc file holds, at its source offset or whole, is reported, as is one whose
map was made from other bytes or other seeds than this run discovers with.
"""
import argparse
import fnmatch
import hashlib
import json
import os
import sys

from . import build, config, sh2
from .recomp import discover

TREATMENTS = ("translated", "interpreted", "data", "replaced", "excluded", "unknown")


def read_treatments(path):
    """[(pattern, treatment, note)] in file order."""
    rows = []
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        if not line.strip() or line.startswith("#"):
            continue
        pattern, treatment, *note = line.rstrip("\n").split("\t")
        if treatment not in TREATMENTS:
            raise SystemExit(f"{path}:{n}: treatment {treatment!r} is not one of {', '.join(TREATMENTS)}")
        rows.append((pattern, treatment, note[0] if note else ""))
    return rows


def treatment(name, rows):
    for pattern, t, note in rows:
        if fnmatch.fnmatchcase(name, pattern):
            return t, note
    return "unknown", ""


def read_map(path):
    """(the header's SHA-1, {address: [(function, kind, file, line)]})."""
    sha1, where = None, {}
    for line in open(path, encoding="utf-8"):
        if line.startswith("#"):
            if "sha1 " in line:
                sha1 = line.rsplit("sha1 ", 1)[1].strip()
            continue
        a, f, kind, name, n = line.split("\t")
        where.setdefault(int(a, 16), []).append((int(f, 16), kind, name, int(n)))
    return sha1, where


def changed(a, b):
    """The (offset, length) runs where equal-length byte strings differ."""
    out, start = [], None
    for i in range(len(a)):
        if a[i] != b[i] and start is None:
            start = i
        elif a[i] == b[i] and start is not None:
            out.append((start, i - start))
            start = None
    if start is not None:
        out.append((start, len(a) - start))
    return out


def image(m, seeds, files, by_sha1, map_dir):
    """One module's row for images.tsv, its gaps, and the problems found."""
    data = open(m.file, "rb").read()
    sha1 = hashlib.sha1(data).hexdigest()
    problems, patches = [], []
    if m.source:
        disc = open(os.path.join(files, m.source.lstrip("/")), "rb").read()
        at = disc[m.source_offset:m.source_offset + len(data)]
        patches = changed(at, data[:len(at)])
        if len(at) < len(data):                    # bytes the preparation added past the file's end
            patches.append((len(at), len(data) - len(at)))
        source = f"{m.source}+{m.source_offset:#x}"
    elif sha1 in by_sha1:
        source = by_sha1[sha1]
    else:
        source = ""
        problems.append(f"{m.name}: no disc file is {os.path.basename(m.file)}, and it names no source")
    p = discover.Program(sh2.Image(data, m.base), [m.base, *seeds])
    map_path = os.path.join(map_dir, m.name + ".tsv")
    mapped = {}
    if not os.path.exists(map_path):
        problems.append(f"{m.name}: no map at {map_path}; recompile first")
    else:
        map_sha1, mapped = read_map(map_path)
        if map_sha1 != sha1:
            problems.append(f"{m.name}: the map was made from other bytes ({map_sha1}); recompile first")
    omitted = sorted(a for a in p.code if a not in mapped)
    if omitted and mapped:
        problems.append(f"{m.name}: {len(omitted)} discovered instructions have no generated line, first {omitted[0]:08X}")
    gaps = p.gaps()
    both = p.code & p.data
    row = {
        "module": m.name, "base": f"{m.base:08X}", "bytes": len(data), "sha1": sha1, "source": source,
        "patches": ",".join(f"{o:#x}+{n}" for o, n in patches),
        "instructions": len(p.code), "omitted": len(omitted), "data halfwords": len(p.data - p.code),
        "code and data": len(both), "unclassified bytes": sum(n for _, n in gaps),
        "unresolved indirect": sum(len(f.unresolved) for f in p.funcs.values()),
    }
    return row, [(m.name, a, a - m.base, n) for a, n in gaps], problems


def inventory(game, log=print):
    inv = game.inventory
    if not inv:
        raise SystemExit("game.toml has no [inventory]")
    manifest = json.load(open(inv.manifest))
    rows = read_treatments(inv.treatments) if inv.treatments else []
    by_sha1 = {v["sha1"]: path for path, v in manifest["files"].items()}
    out = os.path.join(game.build, "inventory")
    os.makedirs(out, exist_ok=True)

    images, gaps, problems = [], [], []
    for name, _, _, seeds in build.specs(game):
        row, g, p = image(game.module(name), seeds, inv.files, by_sha1, os.path.join(game.build, "recomp", "map"))
        images.append(row)
        gaps += g
        problems += p
    made_from = {}
    for row in images:
        made_from.setdefault(row["source"].split("+")[0], []).append(row["module"])

    ip = os.path.join(inv.files, "IP.BIN")
    entries = [("IP.BIN", os.path.getsize(ip) if os.path.exists(ip) else None, manifest.get("ip_sha1", ""))]
    entries += [(path, v["size"], v["sha1"]) for path, v in sorted(manifest["files"].items())]
    entries += [(f"track {n}", 2352 * v["frames"], v["sha1"]) for n, v in sorted(manifest.get("audio", {}).items())]
    files = []
    for path, size, sha1 in entries:
        if path in made_from:
            t, note = "translated", "module " + ", ".join(made_from[path])
        else:
            t, note = treatment(path, rows)
        files.append((path, size, sha1, t, note))

    with open(os.path.join(out, "files.tsv"), "w", encoding="utf-8") as f:
        f.write("# path\tbytes\tsha1\ttreatment\tnote\n")
        f.writelines(f"{p}\t{'' if s is None else s}\t{h}\t{t}\t{n}\n" for p, s, h, t, n in files)
    with open(os.path.join(out, "images.tsv"), "w", encoding="utf-8") as f:
        f.write("# " + "\t".join(images[0]) + "\n")
        f.writelines("\t".join(str(v) for v in row.values()) + "\n" for row in images)
    with open(os.path.join(out, "gaps.tsv"), "w", encoding="utf-8") as f:
        f.write("# module\taddress\toffset\tbytes\n")
        f.writelines(f"{m}\t{a:08X}\t{o:#x}\t{n}\n" for m, a, o, n in gaps)

    summary = summarise(files, images, problems, build.stale(game))
    with open(os.path.join(out, "summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(summary) + "\n")
    log("\n".join(summary))
    log(f"written to {out}")
    return files, images, gaps, problems


def summarise(files, images, problems, stale):
    lines = ["Disc, by treatment:"]
    for t in TREATMENTS:
        chosen = [x for x in files if x[3] == t]
        if chosen:
            lines.append(f"  {t:12} {len(chosen):4} entries {sum(x[1] or 0 for x in chosen):>12,} bytes")
    lines.append("Modules (instructions counted once per address, delay slots included):")
    for r in images:
        lines.append(f"  {r['module']:6} {r['instructions']:7,} instructions, {r['omitted']} without a generated line; "
                     f"{2 * r['data halfwords']:>9,} data bytes, {r['unclassified bytes']:>9,} unclassified; "
                     f"from {r['source'] or '?'}" + (f", patched at {r['patches']}" if r["patches"] else ""))
    if stale:
        lines.append("The build is stale (" + ", ".join(stale) + "): recompile, then run this again.")
    lines.append("Checks: " + ("all passed" if not problems else f"{len(problems)} failed"))
    lines += ["  " + p for p in problems]
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("config")
    a = ap.parse_args(argv)
    *_, problems = inventory(config.load(a.config))
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
