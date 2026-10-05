"""Recompile and build a game from its game.toml, into the build directory it names.

    python -m saturnrecomp.build GAME.toml [--recompile]

Without --recompile it recompiles when anything the generated code is made from has changed since
the last recompile (see `inputs`), and otherwise rebuilds what changed (the runtime, a game layer).
"""
import argparse
import glob
import hashlib
import json
import os
import subprocess

from . import config
from .analysis import resume
from .recomp.__main__ import generate


PACKAGE = os.path.dirname(os.path.abspath(__file__))


def _sha1(path):
    with open(path, "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()


def inputs(game):
    """{input: hash} for what the generated code is made from: each module's file, the config's
    modules, task switch and hooks, the learned seeds, and this package's source, all of it, since
    an unneeded recompile costs less than a stale one."""
    out = {f"module {m.name}": _sha1(m.file) if os.path.exists(m.file) else None for m in game.modules}
    config_part = repr((game.modules, game.tasks, sorted(game.hooks.items())))
    out["game.toml"] = hashlib.sha1(config_part.encode()).hexdigest()
    out["seeds"] = hashlib.sha1(json.dumps(game.learned_seeds(), sort_keys=True).encode()).hexdigest()
    source = hashlib.sha1()
    for path in sorted(glob.glob(os.path.join(PACKAGE, "**", "*.py"), recursive=True)):
        source.update(os.path.relpath(path, PACKAGE).encode() + b"\0" + _sha1(path).encode())
    out["saturnrecomp"] = source.hexdigest()
    return out


def _stamp(game):
    return os.path.join(game.build, "recomp", "inputs.json")


def stale(game):
    """The inputs changed since the last recompile, or why there is nothing to compare; empty if none."""
    if not os.path.exists(game.saturn):
        return ["no build yet"]
    try:
        before = json.load(open(_stamp(game)))
    except (OSError, ValueError):
        return ["no record of the last recompile's inputs"]
    now = inputs(game)
    return [k for k in now if now[k] != before.get(k)] + [k for k in before if k not in now]


def recompile(game, log=print):
    """Recompile every module with its seeds, the learned ones and the resume points, then build."""
    missing = [m.file for m in game.modules if not os.path.exists(m.file)]
    if missing:
        raise SystemExit("missing module files (has the disc been prepared?): " + ", ".join(missing))
    made_from = inputs(game)
    learned = game.learned_seeds()
    points = resume.for_game(game, log)
    specs = []
    for m in game.modules:
        seeds = set(m.seeds) | set(learned.get(m.name, []))
        if game.tasks and m.name == game.tasks.module:
            seeds |= set(points)
        specs.append((m.name, m.file, m.base, sorted(seeds)))
    tasks = (game.tasks.setjmp, game.tasks.longjmp) if game.tasks else None
    out = os.path.join(game.build, "recomp")
    generate(specs, out, log=log, hooks=game.hooks, tasks=tasks)
    subprocess.run(["cmake", "-S", out, "-B", os.path.join(game.build, "recomp-build"), "-G", "Ninja",
                    "-DCMAKE_CXX_COMPILER=clang++"], check=True, capture_output=True)
    build(game)
    with open(_stamp(game), "w") as f:
        json.dump(made_from, f, indent=1, sort_keys=True)
        f.write("\n")


def build(game):
    """Rebuild what changed since the last recompile."""
    r = subprocess.run(["ninja", "-C", os.path.join(game.build, "recomp-build")], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stdout + r.stderr)


def ensure(game, recompile_all=False, log=print):
    """Recompile when asked to or when an input changed, and otherwise rebuild."""
    changed = ["asked to"] if recompile_all else stale(game)
    if changed:
        log("recompiling: " + ", ".join(changed))
        recompile(game, log)
    else:
        build(game)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("config")
    ap.add_argument("--recompile", action="store_true")
    a = ap.parse_args(argv)
    ensure(config.load(a.config), a.recompile)


if __name__ == "__main__":
    main()
