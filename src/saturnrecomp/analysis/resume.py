"""Where a game's tasks can resume: the return address of every call to a function that yields.

A task parks inside a call to the game's yield (or to setjmp) and comes back at that call's return
address. That address is inside a function rather than at an entry, so the build needs it as a
seed. A function yields if it calls one that does, jumps to one as its tail, or runs into one's entry
as part of its own code, so the set grows from the yields the config names.

    python -m saturnrecomp.analysis.resume GAME.toml [--check]
--check compares the points with the seeds the learning loop found, instead of printing them.
"""
import argparse
import sys

from .. import config, sh2
from ..recomp import discover


def _held(p, f, reg):
    """The value `reg` holds all through `f`, when every write to it there is a literal load of that value."""
    values = set()
    for a in f.code:
        ins = p.img.insn(a)
        if reg in discover._writes(ins):
            if ins.op in ("jsr", "jmp", "bsrf", "braf"):     # listed as writes of the register they read
                continue
            if ins.fmt == "mov.l @Rm+,Rn" and ins.m == 15:   # the epilogue's restore
                continue
            if not (ins.op == "mov.l" and ins.size == 4 and ins.target is not None):
                return None
            values.add(p.img.literal(ins))
    return values.pop() if len(values) == 1 else None


def _call_sites(p):
    """(function entry, call address, target or None) for every bsr and jsr reached."""
    for f in p.funcs.values():
        for a in sorted(f.code):
            ins = p.img.insn(a)
            if ins.op == "bsr":
                yield f.entry, a, ins.target
            elif ins.op == "jsr":
                lit = p._literal_for(a, ins.n)
                yield f.entry, a, lit[1] if lit and lit[0] == "lit" else _held(p, f, ins.n)


def resume_points(path, base, seeds, yields, unknown_yields=False, log=None):
    """The sorted return addresses of the calls that can yield, in the program at `path` loaded at `base`."""
    with open(path, "rb") as f:
        p = discover.Program(sh2.Image(f.read(), base), [base, *seeds])
    sites = list(_call_sites(p))

    def may_yield(target):
        return target in yielding or (unknown_yields and target is None)

    yielding = set(yields)
    while True:
        more = {e for e, _, t in sites if may_yield(t)} - yielding
        more |= {f.entry for f in p.funcs.values()
                 if f.entry not in yielding and (f.tails & yielding or (f.code - {f.entry}) & yielding)}
        if not more:
            break
        yielding |= more
    points = sorted({a + 4 for _, a, t in sites if may_yield(t)})
    if log:
        unknown = sum(1 for _, _, t in sites if t is None)
        log(f"{len(yielding)} yielding functions, {len(points)} resume points, {unknown} calls with no known target")
    return points


def for_game(game, log=None):
    """The resume points of the module that switches tasks, or [] when the game has no [tasks]."""
    t = game.tasks
    if not t:
        return []
    m = game.module(t.module)
    seeds = m.seeds + game.learned_seeds().get(m.name, [])
    return resume_points(m.file, m.base, seeds, t.yields, t.unknown_yields, log)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("config")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args(argv)
    game = config.load(a.config)
    points = for_game(game, log=lambda s: print(s, file=sys.stderr))
    if not a.check:
        print(",".join(f"{x:08X}" for x in points))
        return
    learned = game.learned_seeds().get(game.tasks.module, []) if game.tasks else []
    missed = [s for s in learned if s not in set(points)]
    print(f"{len(learned) - len(missed)} of {len(learned)} learned seeds predicted", file=sys.stderr)
    for s in missed:
        print(f"  not predicted: {s:08X}", file=sys.stderr)


if __name__ == "__main__":
    main()
