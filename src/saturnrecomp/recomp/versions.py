"""Compare two versions of one program: the functions they share, the ones changed, and the ones only one has.

    python -m saturnrecomp.recomp.versions A.BIN@BASE B.BIN@BASE [--tsv OUT] [--top N]
    python -m saturnrecomp.recomp.versions --game game.toml --module KRNL B.BIN@BASE [--tsv OUT]

With --game, A is that module as the build recompiles it, seeded with game.toml's seeds and the learned
ones, so its functions are the ones the coverage map counts.

Functions that only moved pair up as in `match`, with only identical pairs voting for the functions
they call, since an edited caller's n-th call need not be the other's. A's seeds are carried to B by their offset from the
nearest pair, and B is discovered again with them. The linker keeps a program's functions in the order of
its sources, so the functions left between the same two pairs on each side are compared with every
address-dependent field masked, and the closest pairs at or above --similar are the changed functions.
"""
import argparse
import bisect
import difflib

from .. import config, sh2
from .discover import Program
from .match import exact, fingerprint, load, propagate


def masked(img, f):
    """The function's instruction words with their branch displacements and pc-relative offsets cleared."""
    out = []
    for a in sorted(f.code):
        w = img.u16(a)
        top = w >> 12
        if top in (0xA, 0xB):                           # bra, bsr
            w &= 0xF000
        elif top in (0x9, 0xD) or w >> 8 in (0x89, 0x8B, 0x8D, 0x8F, 0xC7):   # pc-relative loads, bt/bf, mova
            w &= 0xFF00
        out.append(w)
    return out


def size(f):
    return 2 * len(f.code)


def carry_seeds(seeds, pa, pb, m):
    """A's seeds that are not paired, moved by the offset of the nearest pair at or below each one."""
    anchors = sorted(m)
    out = set()
    for s in seeds:
        if s in m or s not in pa.funcs:
            continue
        k = bisect.bisect_right(anchors, s) - 1
        if k < 0:
            continue
        t = s + m[anchors[k]] - anchors[k]
        if pb.inside(t) and t not in pb.funcs:
            out.add(t)
    return out


def similar_pairs(a, b, m, threshold):
    """{entry in a: (entry in b, similarity)} for the unpaired functions between the same two pairs."""
    (ia, pa), (ib, pb) = a, b
    anchors = sorted(m)
    free_a = sorted(e for e in pa.funcs if e not in m)
    used = set(m.values())
    free_b = sorted(e for e in pb.funcs if e not in used)
    words = {}

    def w(img, p, e):
        if (id(p), e) not in words:
            words[id(p), e] = masked(img, p.funcs[e])
        return words[id(p), e]

    candidates = []
    for x in free_a:
        k = bisect.bisect_right(anchors, x)
        lo = m[anchors[k - 1]] if k > 0 else pb.lo
        hi = m[anchors[k]] if k < len(anchors) else pb.hi
        if lo >= hi:
            continue
        wx = w(ia, pa, x)
        for y in free_b[bisect.bisect_left(free_b, lo):bisect.bisect_left(free_b, hi)]:
            wy = w(ib, pb, y)
            if min(len(wx), len(wy)) < threshold * max(len(wx), len(wy)):
                continue
            r = difflib.SequenceMatcher(None, wx, wy, autojunk=False).ratio()
            if r >= threshold:
                candidates.append((r, x, y))
    out, taken = {}, set()
    for r, x, y in sorted(candidates, reverse=True):
        if x not in out and y not in taken:
            out[x] = (y, r)
            taken.add(y)
    return out


def pair(a, b, threshold):
    """{entry in a: entry in b}: identical functions, the functions identical ones call at the same place,
    and the most similar of the rest between the same two pairs."""
    (ia, pa), (ib, pb) = a, b

    def identical(x, y):
        return fingerprint(ia, pa.funcs[x]) == fingerprint(ib, pb.funcs[y])

    m = propagate(a, b, exact(a, b), identical)
    for x, (y, _) in similar_pairs(a, b, m, threshold).items():
        m[x] = y
    return propagate(a, b, m, identical)


def compare(a, b, seeds=(), threshold=0.6):
    """[(kind, entry in a or None, entry in b or None, bytes in a, bytes in b, similarity)], kind one of
    same, changed, a-only and b-only; and B as discovered with the carried seeds. An unpaired function's
    bytes are its code that no paired function also runs, and one with none (another way into shared
    code) is left out."""
    m = pair(a, b, threshold)
    (ia, pa), (ib, pb) = a, b
    carried = carry_seeds(seeds, pa, pb, m)
    if carried:
        b = (ib, Program(ib, [ib.base, *sorted(carried)]))
        m = pair(a, b, threshold)
        kept = {t for t in carried if t in m.values()}
        if kept != carried:
            b = (ib, Program(ib, [ib.base, *sorted(kept)]))
            m = pair(a, b, threshold)
    ib, pb = b
    rows = []
    for x, y in m.items():
        fa, fb = pa.funcs[x], pb.funcs[y]
        if fingerprint(ia, fa) == fingerprint(ib, fb):
            rows.append(("same", x, y, size(fa), size(fb), 1.0))
        else:
            r = difflib.SequenceMatcher(None, masked(ia, fa), masked(ib, fb), autojunk=False).ratio()
            rows.append(("changed", x, y, size(fa), size(fb), r))
    used = set(m.values())
    shared_a = set().union(*(pa.funcs[x].code for x in m))
    shared_b = set().union(*(pb.funcs[y].code for y in used))
    own_a = {x: 2 * len(f.code - shared_a) for x, f in pa.funcs.items() if x not in m}
    own_b = {y: 2 * len(f.code - shared_b) for y, f in pb.funcs.items() if y not in used}
    rows += [("a-only", x, None, n, 0, 0.0) for x, n in own_a.items() if n]
    rows += [("b-only", None, y, 0, n, 0.0) for y, n in own_b.items() if n]
    return rows, b


def from_game(path, module):
    game = config.load(path)
    mod = next((x for x in game.modules if x.name == module), None)
    if mod is None:
        raise SystemExit(f"{path} has no module {module}")
    seeds = sorted(set(mod.seeds) | set(game.learned_seeds().get(module, [])))
    img = sh2.Image(open(mod.file, "rb").read(), mod.base)
    return (img, Program(img, [mod.base, *seeds])), seeds


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="+", help="A.BIN@BASE B.BIN@BASE, or B.BIN@BASE with --game")
    ap.add_argument("--game", help="game.toml: A is its module --module, seeded as the build seeds it")
    ap.add_argument("--module", help="with --game, the module to compare")
    ap.add_argument("--similar", type=float, default=0.6, help="the least similarity that pairs two changed functions")
    ap.add_argument("--tsv", help="write every row: kind, A entry, B entry, A bytes, B bytes, similarity")
    ap.add_argument("--top", type=int, default=20, help="how many of each kind to list, largest first")
    args = ap.parse_args(argv)
    if args.game:
        if not args.module or len(args.files) != 1:
            ap.error("--game takes --module and B alone")
        a, seeds = from_game(args.game, args.module)
        b = load(args.files[0])
    else:
        if len(args.files) != 2:
            ap.error("give A and B")
        a, b = load(args.files[0]), load(args.files[1])
        seeds = ()
    rows, b = compare(a, b, seeds, args.similar)
    if args.tsv:
        with open(args.tsv, "w", encoding="utf-8") as f:
            for kind, x, y, sa, sb, r in sorted(rows, key=lambda r: (r[1] or 0, r[2] or 0)):
                f.write(f"{kind}\t{x or 0:08X}\t{y or 0:08X}\t{sa}\t{sb}\t{r:.2f}\n")
    pa, pb = a[1], b[1]
    print(f"A: {len(pa.funcs)} functions, {2 * len(pa.code)} bytes of code; B: {len(pb.funcs)}, {2 * len(pb.code)}")
    seen_a, seen_b = set(), set()
    for kind in ("same", "changed", "a-only", "b-only"):
        some = [r for r in rows if r[0] == kind]
        code_a = set().union(*(pa.funcs[r[1]].code for r in some if r[1])) - seen_a
        code_b = set().union(*(pb.funcs[r[2]].code for r in some if r[2])) - seen_b
        seen_a |= code_a
        seen_b |= code_b
        print(f"  {kind:8} {len(some):6} functions, {2 * len(code_a):8} bytes in A, {2 * len(code_b):8} in B")
    for kind, title in (("changed", "changed, by A's size"), ("a-only", "only in A"), ("b-only", "only in B")):
        some = sorted((r for r in rows if r[0] == kind), key=lambda r: -max(r[3], r[4]))[:args.top]
        if some:
            print(f"\n{title}:")
        for _, x, y, sa, sb, r in some:
            print(f"  {x or 0:08X} {y or 0:08X} {sa:6} {sb:6}" + (f"  {r:.0%}" if kind == "changed" else ""))


if __name__ == "__main__":
    main()
