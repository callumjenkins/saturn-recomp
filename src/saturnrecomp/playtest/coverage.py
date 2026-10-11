"""Where the recompiled code no known run has reached sits, and the reached code that leads to it.

A function counts as reached once a run entered it, and code that several functions share (a jump into
the middle of another function is a function of its own) counts once, by instruction address. Each
function no run reached goes to a root:

- behind: called from a function a run reached, on a path no run took;
- pointer: reached only through an address in the program's data (a callback, an object's update);
- orphan: neither called nor pointed at, as the scan for stack-frame prologues finds uncalled code.

A function under roots of more than one kind goes to the first of those, in that order.
"""
import collections
import glob
import os
import re
import struct

from . import review

DEF = re.compile(r"^void f_([0-9A-F]{8})\(SH2Context& c\) \{")
REF = re.compile(r"\bf_([0-9A-F]{8})\(c\)")
INS = re.compile(r"//\s+([0-9A-F]{8}) ")
KINDS = ("behind", "pointer", "orphan")


def generated(recomp, modules):
    """({(module, entry): {instruction addresses}}, {(module, entry): {callees}}) from the recompiler's output."""
    insns, calls = {}, collections.defaultdict(set)
    for module in modules:
        for path in sorted(glob.glob(os.path.join(recomp, f"p_{module.lower()}_[0-9]*.cpp"))):
            cur = None
            for line in open(path):
                d = DEF.match(line)
                if d:
                    cur = (module, int(d.group(1), 16))
                    insns.setdefault(cur, set())
                    continue
                if cur:
                    insns[cur].update(int(a, 16) for a in INS.findall(line))
                    calls[cur].update((module, int(r, 16)) for r in REF.findall(line))
                    calls[cur].discard(cur)
    return insns, calls


def run_name(path):
    """The run a coverage file is from: its directory, or the one above a session's replay."""
    d = os.path.dirname(os.path.abspath(path))
    return os.path.basename(os.path.dirname(d) if os.path.basename(d) == "replay" else d)


def entered(paths, insns):
    """{function: {the runs that entered it}} for the functions of `insns` the coverage files have as run."""
    out = collections.defaultdict(set)
    for path in paths:
        name = run_name(path)
        for line in open(path):
            parts = line.split()
            if len(parts) == 4 and parts[3] == "1":
                k = (parts[0], int(parts[1], 16))
                if k in insns:
                    out[k].add(name)
    return out


def pointed_at(images, insns):
    """The functions whose entry appears as a 32-bit word anywhere in the program images."""
    words = set()
    for b in images:
        for o in (0, 2):
            words.update(struct.unpack_from(f">{(len(b) - o) // 4}I", b, o))
    return {k for k in insns if k[1] in words}


def build(insns, calls, reached, pointed):
    """{"modules": {name: {"code", "reached", KIND: bytes}}, "roots": [(bytes, kind, root, [reached callers])]}; `reached` is any collection of functions."""
    reached = set(reached)
    callers = collections.defaultdict(set)
    for a, bs in calls.items():
        for b in bs:
            callers[b].add(a)

    def below(k):
        seen, todo = {k}, [k]
        while todo:
            for c in calls[todo.pop()]:
                if c not in reached and c not in seen:
                    seen.add(c)
                    todo.append(c)
        return seen

    un = [k for k in insns if k not in reached]
    behind = {k for k in un if callers[k] & reached}
    roots = {
        "behind": sorted(behind, key=lambda k: (-len(below(k)), k)),
        "pointer": sorted(k for k in un if k not in behind and not callers[k] and k in pointed),
        "orphan": sorted(k for k in un if k not in behind and not callers[k] and k not in pointed),
    }
    owner = {}
    for kind in KINDS:
        for r in roots[kind]:
            for k in below(r):
                owner.setdefault(k, (kind, r))

    def addrs(ks):
        return {(k[0], a) for k in ks for a in insns[k]}

    got = addrs(reached)
    modules = collections.defaultdict(collections.Counter)
    for m, _ in addrs(insns):
        modules[m]["code"] += 2
    for m, _ in got:
        modules[m]["reached"] += 2
    groups = collections.defaultdict(list)
    for k, (kind, r) in owner.items():
        groups[(kind, r)].append(k)
    found = []
    for (kind, r), ks in groups.items():
        mine = addrs(ks) - got
        got |= mine                                 # shared code goes to the first group that has it
        if mine:
            modules[r[0]][kind] += 2 * len(mine)
            found.append((2 * len(mine), kind, r, sorted(c for c in callers[r] if c in reached)))
    found.sort(key=lambda f: (-f[0], f[2]))
    return {"modules": {m: dict(c) for m, c in sorted(modules.items())}, "roots": found}


def coverage_map(game, pulled):
    """The map for the game's current recompile, against its own runs and the sessions already reviewed."""
    names = [m.name for m in game.modules]
    insns, calls = generated(os.path.join(game.build, "recomp"), names)
    images = [open(m.file, "rb").read() for m in game.modules if os.path.exists(m.file)]
    runs = entered(review.known_coverage(game, pulled), insns)
    found = build(insns, calls, runs, pointed_at(images, insns))
    found["runs"] = runs
    return found


def report(found, symbols, top=25, modules=None):
    """The map as text: each module's code, how much was reached, and the largest groups no run reached."""
    names = {v: k for k, v in symbols.items()}
    lines = [f"{'module':8}{'code':>9}{'reached':>9}" + "".join(f"{k:>9}" for k in KINDS)]
    for m, c in found["modules"].items():
        pct = 100 * c.get("reached", 0) / c["code"]
        lines.append(f"{m:8}{c['code']:9}{pct:8.0f}%" + "".join(f"{c.get(k, 0):9}" for k in KINDS))
    lines.append("\nthe largest groups no run reached, in bytes, by their root:")
    for n, kind, r, cs in [f for f in found["roots"] if not modules or f[2][0] in modules][:top]:
        via = ", ".join(f"{c[1]:08X}" + (f" {names[c[1]]}" if c[1] in names else "") for c in cs[:3])
        lines.append(f"{n:7}  {kind:8} {r[0]} {r[1]:08X}" + (f"  from {via}" if via else ""))
        if cs and "runs" in found:
            seen = set().union(*(found["runs"][c] for c in cs))
            named = sorted(seen, key=lambda n: (n[:1].isdigit(), n))      # the game's own routes before testers' sessions
            lines.append(f"{'':18}its caller ran in {len(seen)} run{'s' if len(seen) != 1 else ''}: "
                         + ", ".join(named[:4]) + (", ..." if len(seen) > 4 else ""))
    return "\n".join(lines)
