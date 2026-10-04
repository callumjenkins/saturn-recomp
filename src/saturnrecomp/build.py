"""Recompile and build a game from its game.toml, into the build directory it names.

    python -m saturnrecomp.build GAME.toml [--recompile]

Without --recompile it recompiles only when there is no build yet, and otherwise rebuilds what
changed (the runtime, a game layer).
"""
import argparse
import os
import subprocess

from . import config
from .analysis import resume
from .recomp.__main__ import generate


def recompile(game, log=print):
    """Recompile every module with its seeds, the learned ones and the resume points, then build."""
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


def build(game):
    """Rebuild what changed since the last recompile."""
    r = subprocess.run(["ninja", "-C", os.path.join(game.build, "recomp-build")], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stdout + r.stderr)


def ensure(game, recompile_all=False, log=print):
    """Recompile when asked to or when there is no build yet, and otherwise rebuild."""
    if recompile_all or not os.path.exists(game.saturn):
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
