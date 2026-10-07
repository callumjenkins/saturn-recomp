"""What a session found: the functions discovery missed, and the recompiled code no earlier run had reached."""
import glob
import os

from .. import learn

REVIEWED = "reviewed.json"              # in a pulled session once its review is posted


def instructions(path):
    """{(module, address)} for every instruction the run reached, each function taken as its instruction count from its start."""
    reached = set()
    for line in open(path):
        parts = line.split()
        if len(parts) == 4 and parts[3] == "1":
            module, addr, count = parts[0], int(parts[1], 16), int(parts[2])
            reached.update((module, a) for a in range(addr, addr + 2 * count, 2))
    return reached


def known_coverage(game, pulled):
    """The coverage files a session is measured against: the game's own runs and the sessions already reviewed."""
    own = [p for pattern in game.playtest.coverage for p in glob.glob(pattern)]
    reviewed = [os.path.join(d, "coverage.txt") for d in glob.glob(os.path.join(pulled, "*"))
                if os.path.exists(os.path.join(d, REVIEWED)) and os.path.exists(os.path.join(d, "coverage.txt"))]
    return own + reviewed


def missed(game, log_text):
    """["MODULE ADDRESS"] for the code discovery missed that stopped the session; the module is "?" where it cannot be placed."""
    m = learn.MISSED.search(log_text)
    if not m:
        return []
    addr = int(next(g for g in m.groups() if g), 16)
    return [f"{learn.owner(game, addr, log_text) or '?'} {addr:08X}"]


def report(game, session, pulled):
    """The numbers a review posts, and what the reviewer reads first."""
    log_text = open(os.path.join(session, "log.txt"), errors="replace").read()
    coverage = os.path.join(session, "coverage.txt")
    reached = instructions(coverage) if os.path.exists(coverage) else set()
    known = set()
    for path in known_coverage(game, pulled):
        if os.path.dirname(os.path.abspath(path)) != os.path.abspath(session):
            known |= instructions(path)
    return {"missed": missed(game, log_text), "new_bytes": 2 * len(reached - known),
            "reached_bytes": 2 * len(reached), "known_runs": len(known_coverage(game, pulled))}
