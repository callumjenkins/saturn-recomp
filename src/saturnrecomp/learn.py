"""Run a game until it stops; while it stops at code discovery missed, add that address as a seed,
recompile and go again. The seeds go to the file game.toml names.

    python -m saturnrecomp.learn GAME.toml [--out DIR] [--limit N] -- SATURN_ARGS...
SATURN_ARGS are the saturn executable's own (--cue, --headless, --vblanks, --input...), without --out.
"""
import argparse
import os
import re
import shutil
import subprocess

from . import build, config

MISSED = re.compile(r"FATAL\] (?:call to ([0-9A-F]{8}), not an entry of an active module"
                    r"|no function at ([0-9A-F]{8})|task at ([0-9A-F]{8}): no function there)")


def run(game, args, out, timeout=900):
    """One run of the current build into `out`, which it empties first; its log, also in out/log.txt."""
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    p = subprocess.run([game.saturn, "--out", out, *args], capture_output=True, text=True, timeout=timeout)
    log = p.stdout + p.stderr
    with open(os.path.join(out, "log.txt"), "w") as f:
        f.write(log)
    return log


def owner(game, addr, log):
    """The module whose image holds `addr`. Where images overlap, the one the log last saw running."""
    hits = [m.name for m in game.modules if m.base <= addr < m.base + os.path.getsize(m.file)]
    if len(hits) == 1:
        return hits[0]
    polls = re.search(r"last polls in: (.*)", log)
    for name in re.findall(r"(\w+):[0-9A-F]{8}", polls.group(1) if polls else ""):
        if name in hits:
            return name
    return None


def learn(game, args, out, limit=40, log=print):
    """Run, add the missed address as a seed and recompile, up to `limit` times; the last run's log."""
    seeds = game.learned_seeds()
    for i in range(limit):
        text = run(game, args, out)
        m = MISSED.search(text)
        if not m:
            return text
        addr = int(next(g for g in m.groups() if g), 16)
        name = owner(game, addr, text)
        if name is None or addr in seeds.get(name, []):
            why = f"cannot place {addr:08X}" if name is None else f"{addr:08X} already a seed of {name}"
            return text + f"stopped: {why}\n"
        seeds.setdefault(name, []).append(addr)
        game.save_learned_seeds(seeds)
        log(f"iteration {i + 1}: seed {name} {addr:08X}")
        build.recompile(game, log=lambda s: None)
    return text


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("config")
    ap.add_argument("--out")
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("saturn_args", nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    game = config.load(a.config)
    build.ensure(game)
    args = a.saturn_args[1:] if a.saturn_args[:1] == ["--"] else a.saturn_args
    print(learn(game, args, a.out or os.path.join(game.build, "run1"), a.limit, log=lambda s: print(s, flush=True)))


if __name__ == "__main__":
    main()
