"""Match functions between programs that link the same code at other addresses.

    python -m saturnrecomp.recomp.match A.BIN@0600B000 B.BIN@0600B000 [--names names.tsv] [--out b-names.tsv]

A game made of several programs (overlays, or complete programs swapped at
one address) repeats its engine in each, shifted. A function's fingerprint
is its code with everything that depends on where it was linked masked
out: the displacement of `bsr` (a call to another function), and the
values in its literal pool (addresses of data and callees). What is left,
its instructions in order, pc-relative literal *positions* included, is
the same wherever the linker put it.

A fingerprint shared by exactly one function on each side is a match.
Then the matches vote for their neighbours: two unmatched functions called
from the same place in two matched ones (the n-th call of each) are a
match too, repeated to a fixed point. A names file (address TAB name)
given for one side is carried to the other.
"""
import argparse
import collections
import hashlib

from .. import sh2
from .discover import Program


def fingerprint(img, f):
    words = []
    for a in sorted(f.code):
        w = img.u16(a)
        if w >> 12 == 0xB:                   # bsr: mask the displacement
            w = 0xB000
        words.append(w)
    return hashlib.sha1(b"".join(x.to_bytes(2, "big") for x in words)).hexdigest()


def ordered_calls(img, p, f):
    """The callees of `f` in the order its code calls them (bsr and literal jsr)."""
    out = []
    for a in sorted(f.code):
        ins = img.insn(a)
        if ins.op == "bsr":
            out.append(ins.target)
        elif ins.op == "jsr":
            r = p._literal_for(a, ins.n)
            out.append(r[1] if r and r[0] == "lit" else None)
    return out


def load(spec, seeds=()):
    path, _, base = spec.partition("@")
    base = int(base or "0600B000", 16)
    img = sh2.Image(open(path, "rb").read(), base)
    return img, Program(img, [base] + list(seeds))


def match(a, b):
    """{entry in a: entry in b}."""
    (ia, pa), (ib, pb) = a, b
    fa = collections.defaultdict(list)
    fb = collections.defaultdict(list)
    for e, f in pa.funcs.items():
        fa[fingerprint(ia, f)].append(e)
    for e, f in pb.funcs.items():
        fb[fingerprint(ib, f)].append(e)
    m = {}
    for h, ea in fa.items():
        eb = fb.get(h)
        if len(ea) == 1 and eb and len(eb) == 1:
            m[ea[0]] = eb[0]
    # propagate through call sites
    changed = True
    while changed:
        changed = False
        used = set(m.values())
        for x, y in list(m.items()):
            ca = ordered_calls(ia, pa, pa.funcs[x])
            cb = ordered_calls(ib, pb, pb.funcs[y])
            if len(ca) != len(cb):
                continue
            for s, t in zip(ca, cb):
                if s is None or t is None or s in m or t in used:
                    continue
                if s in pa.funcs and t in pb.funcs:
                    m[s] = t
                    used.add(t)
                    changed = True
    return m


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("a", help="FILE@BASE")
    ap.add_argument("b", help="FILE@BASE")
    ap.add_argument("--names", help="names for A: hex address TAB name")
    ap.add_argument("--out", help="write the names carried to B")
    args = ap.parse_args(argv)
    A, B = load(args.a), load(args.b)
    m = match(A, B)
    print("A: %d functions, B: %d functions, matched %d" % (len(A[1].funcs), len(B[1].funcs), len(m)))
    moved = sum(1 for x, y in m.items() if x != y)
    print("  %d at the same address, %d moved" % (len(m) - moved, moved))
    if args.names:
        names = {}
        for line in open(args.names, encoding="utf-8"):
            if line.strip() and not line.startswith("#"):
                addr, name = line.split("\t")[:2]
                names[int(addr, 16)] = name.strip()
        out = []
        for addr, name in sorted(names.items()):
            if addr in m:
                out.append((m[addr], name))
            else:
                print("  not carried: %08X %s" % (addr, name))
        if args.out:
            with open(args.out, "w", encoding="utf-8") as fo:
                for addr, name in sorted(out):
                    fo.write("%08X\t%s\n" % (addr, name))
        print("  %d of %d names carried" % (len(out), len(names)))


if __name__ == "__main__":
    main()
