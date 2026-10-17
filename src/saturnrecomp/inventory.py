"""What a game's disc holds, and what became of each part.

    python -m saturnrecomp.inventory GAME.toml

Reads game.toml's [inventory] and the map the last recompile wrote (BUILD/recomp/map). Writes into
BUILD/inventory:

    files.tsv     every file and audio track on the disc, and the system area (IP.BIN): its size,
                  SHA-1, treatment, the note, and what names it. A file a module is made from is
                  "translated", naming the modules; any other takes the first row of the treatments
                  file that matches it, or is "unknown". "Named by" lists the modules, and the text
                  files something names in turn, that hold a form of the file's name: "full" (the
                  name), "%d" (its digits as %d), "stem" (no extension) or "letters" (the stem's
                  leading letters, four or more, alone or with %d). A form other than the full name
                  counts only as a whole word, and a short word can be a coincidence: weigh it.
    images.tsv    each module: its file's SHA-1, the disc file it comes from, the byte ranges
                  preparation changed or added, and its halfwords by class: instructions, those of them with
                  no generated line, data and unclassified.
    identity.json what this inventory describes: the build's own record of its inputs (each module's
                  bytes, game.toml's modules, the seeds and saturnrecomp's source), the executable's
                  SHA-1, and the manifest's and treatments file's.
    gaps.tsv      each unclassified run: module, guest address, file offset, length, and evidence for
                  what it holds: its content (zero, fill of nop or FFFF halfwords, text or other),
                  the prologues in it (mov.l
                  r14,@-r15 then sts.l pr,@-r15, as SH-2 compilers begin a function that calls), the
                  literals in discovered code that point into it, and how many instructions a descent
                  from its start (past fill, at a boundary) decodes cleanly within it before it
                  returns: an uncalled function with no frame, or a record that happens to decode.
    sound.tsv     each 68000 program [[inventory.sound]] lists, which the runtime interprets on
                  Musashi: where on the disc it comes from, its SHA-1, the vectors the descent starts
                  from, its bytes by class (instructions, data, unclassified), the jump tables read,
                  the jumps through a register left unresolved, and where a path ended badly
                  (saturnrecomp.m68k).
    sound-gaps.tsv each of those programs' unclassified runs: program, address, file offset, length,
                  content, the rts and rte words in it, the addresses discovered code names in it,
                  and how many instructions a clean descent from its first word past fill decodes
                  to a return.
    summary.txt   the totals, each module's unclassified bytes by what the gap report found in them,
                  and each check that failed.

The treatments file is TAB-separated: PATTERN, TREATMENT, then a note giving the evidence. A pattern
is a disc path glob ("/BOMSS/*.CPK"), "track NN" for an audio track or "IP.BIN" for the system area.
A pattern ending in "?named" matches only files something names (files.tsv's last column).
A glob with "@START-END" (hex byte offsets, END exclusive) treats that part of each file it matches;
files.tsv then has a row per part, "PATH@START-END", and the bytes no part covers take the file's own
treatment. A treatment is one of TREATMENTS. Lines starting with # are comments.

Instructions are counted once per address however many functions hold them, delay slots included.
A 68000 program whose bytes the treatments file does not treat as one part, "interpreted", is reported.
A module whose image no disc file holds, at its source offset or whole, is reported, as is one whose
map was made from other bytes or other seeds than this run discovers with.
"""
import argparse
import bisect
import fnmatch
import hashlib
import json
import os
import re
import sys

from . import build, config, m68k, sh2
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


def treatment(name, rows, named=False):
    for pattern, t, note in rows:
        glob, _, cond = pattern.partition("?")
        if "@" not in glob and fnmatch.fnmatchcase(name, glob) and (cond != "named" or named):
            return t, note
    return "unknown", ""


WORD = re.compile(rb"[A-Z0-9_%]+")


def forms(path):
    """{the forms of a disc file's name a program can hold, upper case: kind}."""
    name = os.path.basename(path).upper()
    stem = os.path.splitext(name)[0]
    letters = re.match(r"[A-Z_]*", stem).group()
    out = {}
    if len(letters) >= 4:
        out[letters] = out[letters + "%D"] = "letters"
    out[stem] = "stem"
    out[re.sub(r"\d+", "%D", name)] = "%d"
    out[name] = "full"
    return {k.encode(): v for k, v in out.items()}


def text_index(data):
    upper = data.upper()
    return upper, set(WORD.findall(upper))


def named_by(path, sources):
    """["SOURCE:KIND"] for each source holding a form of the file's name, the strongest form each."""
    out = []
    want = forms(path)
    full = os.path.basename(path).upper().encode()
    for source, (upper, words) in sources.items():
        if full in upper:
            out.append(f"{source}:full")
        else:
            kinds = [want[w] for w in want if w in words]
            if kinds:
                out.append(f"{source}:{min(kinds, key=['full', '%d', 'stem', 'letters'].index)}")
    return out


def naming(paths, modules, files):
    """{disc path: named_by}, with each text file named by something a source in turn, to a fixed point."""
    sources = {m.name: text_index(open(m.file, "rb").read()) for m in modules}
    found = {}
    while True:
        found = {p: n for p in paths if (n := named_by(p, sources))}
        more = {p for p in found if p not in sources and os.path.exists(f := os.path.join(files, p.lstrip("/")))
                and content(open(f, "rb").read()) == "text"}
        if not more:
            return found
        for p in more:
            sources[p] = text_index(open(os.path.join(files, p.lstrip("/")), "rb").read())


def parts(path, size, rows, named=False):
    """[(name, bytes, treatment, note)] for a file: a row per part a ranged pattern treats, in offset
    order, and one per stretch between them with the file's own treatment."""
    ranged = []
    for pattern, t, note in rows:
        glob, _, span = pattern.partition("@")
        if span and fnmatch.fnmatchcase(path, glob):
            lo, hi = (int(x, 16) for x in span.split("-"))
            ranged.append((lo, min(hi, size), t, note))
    if not ranged:
        return [(path, size, *treatment(path, rows, named))]
    out, at = [], 0
    for lo, hi, t, note in sorted(ranged):
        if lo > at:
            out.append((f"{path}@{at:#x}-{lo:#x}", lo - at, *treatment(path, rows, named)))
        out.append((f"{path}@{lo:#x}-{hi:#x}", hi - lo, t, note))
        at = max(at, hi)
    if at < size:
        out.append((f"{path}@{at:#x}-{size:#x}", size - at, *treatment(path, rows, named)))
    return out


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


PROLOGUE = bytes.fromhex("2FE64F22")


def content(chunk):
    if not chunk.strip(b"\0"):
        return "zero"
    if not len(chunk) & 1 and {chunk[i:i + 2] for i in range(0, len(chunk), 2)} <= {b"\x00\x09", b"\xff\xff", b"\0\0"}:
        return "fill"
    printable = sum(32 <= c < 127 or c in (0, 9, 10, 13) for c in chunk)
    return "text" if len(chunk) >= 8 and printable >= 0.9 * len(chunk) else "other"


def evidence(data, base, gaps, literals, descends=lambda a, n: 0):
    """(content, prologues, literals pointing in, instructions a clean descent decodes) for each
    (address, length) gap."""
    lits = sorted(literals)
    out = []
    for a, n in gaps:
        chunk = data[a - base:a - base + n]
        prologues = sum(1 for i in range(0, n - 3, 2) if chunk[i:i + 4] == PROLOGUE)
        pointers = bisect.bisect_left(lits, a + n) - bisect.bisect_left(lits, a)
        kind = content(chunk)
        out.append((kind, prologues, pointers, descends(a, n) if kind == "other" else 0))
    return out


def descent(p):
    """For a gap (address, length): the instructions a descent from its start decodes, within the gap
    and clashing with nothing classified, if it starts at a boundary and returns; else 0."""
    def count(a, n):
        end = a + n
        while a < end and p.img.u16(a) in (0x0000, 0x0009, 0xFFFF):
            a += 2
        if a >= end or not p._boundary(a):
            return 0
        f, data = p._descend(a, commit=False)
        if f.bad or f.code & p.data or data & p.code or any(not a <= c < end for c in f.code) or f.calls - p.funcs.keys():
            return 0
        returns = any(p.img.insn(c).op in ("rts", "rte") for c in f.code)
        return len(f.code) if returns else 0
    return count


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
    found = evidence(data, m.base, gaps, p._literal_values(), descent(p))
    return row, [(m.name, a, a - m.base, n, *e) for (a, n), e in zip(gaps, found)], problems


def sound_image(s, files, rows, lib):
    """One 68000 program's row for sound.tsv, its gaps, and the problems found. `rows`: files.tsv's."""
    data = s.read(files)
    p = m68k.Program(m68k.Image(data, lib))
    code = p.code_bytes()
    gaps = m68k.evidence(p)
    end = s.offset + len(data)
    part = f"{s.source}@{s.offset:#x}-{end:#x}"
    found = [r for r in rows if r[0] == part or r[0] == s.source and s.offset == 0 and r[1] == end]
    problems = []
    if not found:
        problems.append(f"{s.name}: the treatments file has no part {part}")
    elif found[0][3] != "interpreted":
        problems.append(f"{s.name}: {found[0][0]} is treated as {found[0][3]}, not interpreted")
    row = {
        "program": s.name, "source": f"{s.source}+{s.offset:#x}", "bytes": len(data),
        "sha1": hashlib.sha1(data).hexdigest(), "entries": " ".join(f"{e:X}" for e in p.entries),
        "instructions": len(p.code), "code bytes": len(code), "data bytes": len(p.data - code),
        "code and data": len(code & p.data), "unclassified bytes": sum(n for _, n, *_ in gaps),
        "tables": len(p.tables), "unresolved indirect": len(p.unresolved),
        "bad": ",".join(f"{a:X}" for a in sorted(p.bad)),
    }
    return row, [(s.name, a, a + s.offset, n, *e) for a, n, *e in gaps], problems


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
    names = naming([p for p, _, _ in entries if p.startswith("/")], game.modules, inv.files)
    files = []
    for path, size, sha1 in entries:
        by = ", ".join(names.get(path, []))
        if path in made_from:
            files.append((path, size, sha1, "translated", "module " + ", ".join(made_from[path]), by))
        else:
            files += [(name, n, sha1, t, note, by) for name, n, t, note in parts(path, size or 0, rows, bool(by))]

    sound, sound_gaps = [], []
    if inv.sound:
        lib = m68k.library(os.path.join(game.build, "m68kdasm"))
        for s in inv.sound:
            row, g, p = sound_image(s, inv.files, files, lib)
            sound.append(row)
            sound_gaps += g
            problems += p
        with open(os.path.join(out, "sound.tsv"), "w", encoding="utf-8") as f:
            f.write("# " + "\t".join(sound[0]) + "\n")
            f.writelines("\t".join(str(v) for v in row.values()) + "\n" for row in sound)
        with open(os.path.join(out, "sound-gaps.tsv"), "w", encoding="utf-8") as f:
            f.write("# program\taddress\toffset\tbytes\tcontent\treturns\tpointers in\tdecodes\n")
            f.writelines("\t".join([m, f"{a:05X}", f"{o:#x}", *map(str, rest)]) + "\n" for m, a, o, *rest in sound_gaps)

    with open(os.path.join(out, "files.tsv"), "w", encoding="utf-8") as f:
        f.write("# path\tbytes\tsha1\ttreatment\tnote\tnamed by\n")
        f.writelines(f"{p}\t{'' if s is None else s}\t{h}\t{t}\t{n}\t{b}\n" for p, s, h, t, n, b in files)
    with open(os.path.join(out, "images.tsv"), "w", encoding="utf-8") as f:
        f.write("# " + "\t".join(images[0]) + "\n")
        f.writelines("\t".join(str(v) for v in row.values()) + "\n" for row in images)
    with open(os.path.join(out, "gaps.tsv"), "w", encoding="utf-8") as f:
        f.write("# module\taddress\toffset\tbytes\tcontent\tprologues\tpointers in\tdecodes\n")
        f.writelines("\t".join([m, f"{a:08X}", f"{o:#x}", *map(str, rest)]) + "\n" for m, a, o, *rest in gaps)

    identity = {"build": _json(os.path.join(game.build, "recomp", "inputs.json")),
                "executable": _sha1_file(game.saturn), "manifest": _sha1_file(inv.manifest),
                "treatments": _sha1_file(inv.treatments) if inv.treatments else None}
    with open(os.path.join(out, "identity.json"), "w", encoding="utf-8") as f:
        json.dump(identity, f, indent=1, sort_keys=True)
        f.write("\n")
    summary = summarise(files, images, gaps, problems, build.stale(game), sound, sound_gaps)
    with open(os.path.join(out, "summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(summary) + "\n")
    log("\n".join(summary))
    log(f"written to {out}")
    return files, images, gaps, problems, sound


def _sha1_file(path):
    return hashlib.sha1(open(path, "rb").read()).hexdigest() if os.path.exists(path) else None


def _json(path):
    return json.load(open(path)) if os.path.exists(path) else None


def gap_kind(content, prologues, pointers, descends):
    """What a gap's evidence suggests, for the summary."""
    if prologues:
        return "prologue"
    if content in ("text", "zero", "fill"):
        return content
    if pointers:
        return "pointed into"
    return "decodes" if descends >= 4 else "nothing"


def summarise(files, images, gaps, problems, stale, sound=(), sound_gaps=()):
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
    lines.append("Unclassified bytes by what is in them (text; zeros; nop or FFFF fill; a prologue; code points"
                 " into the run; it decodes cleanly to a return, 4 instructions or more; nothing):")
    kinds = ("text", "zero", "fill", "prologue", "pointed into", "decodes", "nothing")
    for r in images:
        by = {k: 0 for k in kinds}
        for m, _, _, n, *e in gaps:
            if m == r["module"]:
                by[gap_kind(*e)] += n
        if any(by.values()):
            lines.append(f"  {r['module']:6} " + ", ".join(f"{k} {v:,}" for k, v in by.items()))
    if sound:
        same = {}
        for r in sound:
            same.setdefault(r["sha1"], []).append(r["program"])
        lines.append(f"68000 programs, interpreted on Musashi ({len(same)} different, "
                     f"{sum(r['bytes'] for r in {r['sha1']: r for r in sound}.values()):,} bytes counted once):")
        for r in sound:
            first = same[r["sha1"]][0]
            lines.append(f"  {r['program']:6} {r['instructions']:7,} instructions in {r['code bytes']:,} bytes; "
                         f"{r['data bytes']:,} data bytes, {r['unclassified bytes']:,} unclassified; {r['tables']} tables, "
                         f"{r['unresolved indirect']} unresolved, {len(r['bad'].split(',')) if r['bad'] else 0} bad; "
                         f"from {r['source']}" + (f", the same bytes as {first}" if first != r["program"] else ""))
        lines.append("Unclassified 68000 bytes by what is in them (text; zeros; nop, FFFF or zero fill; an rts or rte;"
                     " code points into the run; it decodes cleanly to a return, 4 instructions or more; nothing):")
        kinds = ("text", "zero", "fill", "returns", "pointed into", "decodes", "nothing")
        for r in sound:
            by = {k: 0 for k in kinds}
            for m, _, _, n, *e in sound_gaps:
                if m == r["program"]:
                    k = gap_kind(*e)
                    by["returns" if k == "prologue" else k] += n
            if any(by.values()):
                lines.append(f"  {r['program']:6} " + ", ".join(f"{k} {v:,}" for k, v in by.items()))
    if stale:
        lines.append("The build is stale (" + ", ".join(stale) + "): recompile, then run this again.")
    lines.append("Checks: " + ("all passed" if not problems else f"{len(problems)} failed"))
    lines += ["  " + p for p in problems]
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("config")
    a = ap.parse_args(argv)
    problems = inventory(config.load(a.config))[3]
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
