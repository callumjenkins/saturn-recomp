"""Where a game's tasks can resume: the return address of every call to a function that yields.

A task parks inside a call to the game's yield (or to setjmp) and comes back at that call's return
address. That address is inside a function rather than at an entry, so the build needs it as a
seed. A function yields if it calls one that does, jumps to one as its tail, or runs into one's entry
as part of its own code, so the set grows from the yields the config names. A program that switches
tasks by swapping stacks ([[task_switch]]) yields in the functions holding its save and switch too.
A task resumed on a fiber other than the one it parked on runs from there up its whole call chain.

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


def analyse(path, base, seeds, yields, unknown_yields=False, inside=()):
    """(the entries of the functions that can yield, the sorted return addresses of the calls that can),
    in the program at `path` loaded at `base`. The functions whose code holds an address in `inside` yield
    as well as those in `yields`."""
    with open(path, "rb") as f:
        p = discover.Program(sh2.Image(f.read(), base), [base, *seeds])
    sites = list(_call_sites(p))

    def may_yield(target):
        return target in yielding or (unknown_yields and target is None)

    yielding = set(yields) | {f.entry for f in p.funcs.values() if f.code & set(inside)}
    while True:
        more = {e for e, _, t in sites if may_yield(t)} - yielding
        more |= {f.entry for f in p.funcs.values()
                 if f.entry not in yielding and (f.tails & yielding or (f.code - {f.entry}) & yielding)}
        if not more:
            break
        yielding |= more
    points = sorted({a + 4 for _, a, t in sites if may_yield(t)})
    return yielding, points, sum(1 for _, _, t in sites if t is None)


def resume_points(path, base, seeds, yields, unknown_yields=False, log=None, inside=()):
    """The sorted return addresses of the calls that can yield, in the program at `path` loaded at `base`."""
    yielding, points, unknown = analyse(path, base, seeds, yields, unknown_yields, inside)
    if log:
        log(f"{len(yielding)} yielding functions, {len(points)} resume points, {unknown} calls with no known target")
    return points


def yielding_functions(game):
    """The entries of the functions that can yield, in the module [tasks] names. A replacement for one of
    these cannot be parked and resumed on another fiber, so it has to leave them alone."""
    t = game.tasks
    if not t:
        return set()
    m = game.module(t.module)
    return analyse(m.file, m.base, m.seeds + game.learned_seeds().get(m.name, []), t.yields, t.unknown_yields)[0]


def for_game(game, log=None):
    """{module name: its resume points}, for the module [tasks] names and each with a [[task_switch]]."""
    out = {}
    learned = game.learned_seeds()
    t = game.tasks
    if t:
        m = game.module(t.module)
        out[m.name] = resume_points(m.file, m.base, m.seeds + learned.get(m.name, []), t.yields, t.unknown_yields, log)
    for name in dict.fromkeys(s.module for s in game.task_switches):
        m = game.module(name)
        switches = [s for s in game.task_switches if s.module == name]
        points = resume_points(m.file, m.base, m.seeds + learned.get(name, []), [y for s in switches for y in s.yields],
                               any(s.unknown_yields for s in switches), log,
                               inside=[a for s in switches for a in (s.save, s.switch)])
        out[name] = sorted(set(out.get(name, [])) | set(points))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("config")
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args(argv)
    game = config.load(a.config)
    for name, points in for_game(game, log=lambda s: print(s, file=sys.stderr)).items():
        if not a.check:
            print(f"{name}: " + ",".join(f"{x:08X}" for x in points))
            continue
        learned = game.learned_seeds().get(name, [])
        missed = [s for s in learned if s not in set(points)]
        print(f"{name}: {len(learned) - len(missed)} of {len(learned)} learned seeds predicted", file=sys.stderr)
        for s in missed:
            print(f"  not predicted: {s:08X}", file=sys.stderr)


if __name__ == "__main__":
    main()
