"""The recompiler's self-test: the interpreter records, the recompiled code replays.

    python -m saturnrecomp.recomp.selftest --out VECTORS.txt --image NAME=FILE@BASE [--image ...]
                                        --test NAME [--auto] [--funcs A,B,...] [--names TSV]
                                        [--vectors 16] [--seed 1]
    python -m saturnrecomp.recomp.selftest --optest DIR

A vectors file (the format is in runtime/src/app/selftest.cpp) says which images to
load and which modules to activate, then, function by function, the full
register state before and after each call as sh2emu computed it, and the
crc32 of both work RAMs after the function's vectors. The `selftest`
executable of a generated build replays them on the recompiled code.

Game functions: `--funcs` names them; `--auto` takes every function of the
tested module that, on a few random register states, returns without
touching anything but its stack (no hardware, no pointer from a random
argument): the arithmetic, fixed-point and table helpers; failing that,
every function that does the same with r4-r7 pointing into random data
(and writes only there): the routines on vectors, matrices and structures.
A vector that faults in the interpreter is left out, and its writes are
undone.

The instruction test (`--optest`, or `python -m saturnrecomp.recomp --optest`)
is saturn-recomp's own: a synthetic program with every instruction form the
decoder knows, alone and in a delay slot, with random registers, operands
and memory; and sequences for the control flow (conditional and delayed
branches, a branch into its own slot, loops, bsr/jsr/bsrf, a tail jmp, an
SHC switch, rts reading PR before its slot writes it, division steps).
"""
import argparse
import os
import random
import zlib

from .. import sh2
from .. import sh2emu
from . import discover

SENTINEL = sh2emu.RETURN_SENTINEL
M32 = 0xFFFFFFFF
EDGE = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
        0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF]
STACK_TOP = 0x002FFFF0            # the tests' stack: the top of WRAM-L
SCRATCH = 0x00280000              # random data the pointer arguments point into (mode args)
SCRATCH_SIZE = 0x40000


class JournalMemory(sh2emu.Memory):
    """sh2emu's memory with an undo journal, so a vector that faults leaves
    no trace (the recompiled side never runs it)."""

    def __init__(self):
        super().__init__()
        self.journal = []

    def write(self, addr, size, value):
        buf, o = self._find(addr, size)
        self.journal.append((buf, o, bytes(buf[o:o + size])))
        super().write(addr, size, value)

    def undo(self):
        for buf, o, old in reversed(self.journal):
            buf[o:o + len(old)] = old
        self.journal = []


def new_memory(images):
    """Both work RAMs, zero, with the images (data, base) copied in: what
    selftest.cpp starts from."""
    mem = JournalMemory()
    regions = {0x06000000: mem.add(0x06000000, 0x100000), 0x00200000: mem.add(0x00200000, 0x100000)}
    for data, base in images:
        lo = base & 0xFFF00000
        buf = regions[lo]
        buf[base - lo:base - lo + len(data)] = data
    return mem


def crcs(mem):
    by = {lo: buf for lo, _, buf in mem.regions}
    return zlib.crc32(by[0x00200000]), zlib.crc32(by[0x06000000])


def run(mem, entry, st, max_steps):
    cpu = sh2emu.CPU(mem)
    cpu.r = list(st[:16])
    cpu.sr = st[16]
    cpu.gbr, cpu.vbr, cpu.mach, cpu.macl = st[17:21]
    cpu.call(entry, max_steps=max_steps)
    return cpu.r[:] + [cpu.sr, cpu.gbr, cpu.vbr, cpu.mach, cpu.macl, cpu.pr]


def rand_word(rng):
    k = rng.random()
    if k < 0.3:
        return rng.getrandbits(32)
    if k < 0.5:
        return rng.randint(-16, 16) & M32
    if k < 0.7:
        return rng.choice(EDGE)
    return rng.getrandbits(16)


def rand_state(rng, mode="any"):
    """22 words: r0-r15, sr, gbr, vbr, mach, macl, pr. Modes: any (random),
    game (r15 on the tests' stack), args (and r4-r7 pointers into the scratch
    data), ptr (every register and GBR a pointer into the optest blob), half
    (pointers to half the blob's address, for @(r0,Rn): the sum lands in
    it), stack (r15 into the blob), loop (and r1 a small count), switch (and
    r0 a small index)."""
    w = [rand_word(rng) for _ in range(16)]
    gbr = rand_word(rng)
    if mode in ("game", "args"):
        w[15] = STACK_TOP
        if mode == "args":
            for i in range(4, 8):
                w[i] = SCRATCH + rng.randrange(0x100, SCRATCH_SIZE - 0x1000, 4)
    elif mode == "ptr":
        w = [_ptr(rng) for _ in range(16)]
        gbr = _ptr(rng)
    elif mode == "half":
        w = [_half(rng) for _ in range(16)]
        gbr = _half(rng)
    elif mode in ("stack", "loop", "switch"):
        w[15] = _ptr(rng)
        if mode == "loop":
            w[1] = rng.randint(1, 40)
        if mode == "switch":
            w[0] = rng.randint(0, 5)
    return w + [rng.getrandbits(32) & 0x3F3, gbr, rng.getrandbits(32), rand_word(rng), rand_word(rng), SENTINEL]


def record(mem, entry, label, states, max_steps=200_000):
    """The vector lines for one function (faulting states left out)."""
    lines = ["func %08X %s" % (entry, label)]
    for st in states:
        mem.journal = []
        try:
            out = run(mem, entry, st, max_steps)
        except (sh2emu.MemoryError_, RuntimeError):
            mem.undo()
            continue
        lines.append("v " + " ".join("%08X" % x for x in st + out))
    mem.journal = []
    lo, hi = crcs(mem)
    lines.append("mem %08X %08X" % (lo, hi))
    return lines, len(lines) - 2


def _own(a):
    return STACK_TOP - 0x10000 <= a < STACK_TOP + 0x100 or SCRATCH <= a < SCRATCH + SCRATCH_SIZE


def pure_functions(images, prog, rng, trials=6, max_steps=20_000):
    """{entry: mode} of the functions that return on random states writing
    only their stack and (mode args) what their pointer arguments point at."""
    out = {}
    for e in sorted(prog.funcs):
        for mode in ("game", "args"):
            mem = new_memory(images)
            mem.log = []
            ok = True
            for _ in range(trials):
                mem.log.clear()
                try:
                    run(mem, e, rand_state(rng, mode), max_steps)
                except (sh2emu.MemoryError_, RuntimeError):
                    ok = False
                    break
                if any(k == "w" and not _own(a) for k, a, _, _ in mem.log):
                    ok = False
                    break
            if ok:
                out[e] = mode
                break
    return out


def scratch_data(seed=1):
    rng = random.Random(seed * 104729)
    return bytes(rng.getrandbits(8) for _ in range(SCRATCH_SIZE))


# ---- the instruction test -------------------------------------------------------------
OPTEST_BASE = 0x06040000          # the code (module OPTEST)
BLOB = 0x00240000                 # random data in WRAM-L the tests read and write
BLOB_SIZE = 0x40000
HALF = BLOB // 2


def _ptr(rng):
    return BLOB + rng.randrange(0x100, BLOB_SIZE - 0x1000, 4)


def _half(rng):
    return HALF + rng.randrange(0x100, BLOB_SIZE // 2 - 0x1000, 4)


class Asm:
    """A few lines of SH-2 with labels: op(fmt, n, m, low | to=label),
    long(value | label), word(value | fn(labels)), align()."""

    def __init__(self, base):
        self.base, self.items, self.labels = base, [], {}

    def here(self):
        return self.base + sum(2 if k[0] != "long" else 4 for k in self.items)

    def label(self, name):
        self.labels[name] = self.here()

    def op(self, fmt, n=0, m=0, low=0, to=None):
        self.items.append(("op", fmt, n, m, low, to))

    def word(self, v):
        self.items.append(("word", v))

    def long(self, v):
        self.items.append(("long", v))

    def align(self):
        if self.here() & 3:
            self.op("nop")

    def assemble(self):
        out, pc = bytearray(), self.base
        for it in self.items:
            if it[0] == "op":
                _, fmt, n, m, low, to = it
                if to is not None:
                    t = self.labels[to]
                    kind = sh2._BY_FMT[fmt][4]
                    if "pcl" in kind:
                        low = (t - ((pc & ~3) + 4)) // 4
                    else:
                        low = (t - (pc + 4)) // 2
                w = sh2.encode(fmt, n, m, low)
                assert to is None or sh2.decode(w, pc).target == self.labels[to], (fmt, to)
                out += w.to_bytes(2, "big")
                pc += 2
            elif it[0] == "word":
                v = it[1](self.labels) if callable(it[1]) else it[1]
                out += (v & 0xFFFF).to_bytes(2, "big")
                pc += 2
            else:
                v = self.labels[it[1]] if isinstance(it[1], str) else it[1]
                out += (v & M32).to_bytes(4, "big")
                pc += 4
        return bytes(out)


SKIP = {"bt", "bf", "bt/s", "bf/s", "bra", "bsr", "jmp", "jsr", "braf", "bsrf", "rts", "rte",
        "sleep", "trapa"}


def _data_word(rng):
    """Random data that discovery will not take for code: no prologue."""
    while True:
        w = rng.getrandbits(16)
        if not discover.Program._is_prologue(w) and w >> 8 not in (0x06, 0x26):
            return w


def _variants(fmt, rng, k=3):
    """(n, m, low) for k variants of a format: the same register twice, r0,
    random; displacements and immediates in range for the blocks."""
    _, _, op, _, kind, nsh, msh = sh2._BY_FMT[fmt]
    out = []
    for i in range(k):
        n = m = 0
        if nsh is not None and msh is not None:
            n, m = [(3, 3), (0, rng.randrange(1, 16)), (rng.randrange(16), rng.randrange(16))][i % 3]
        elif nsh is not None:
            n = [0, rng.randrange(1, 16), rng.randrange(16)][i % 3]
        elif msh is not None:
            m = [0, rng.randrange(1, 16), rng.randrange(16)][i % 3]
        if "pcw" in kind:
            low = rng.randint(1, 10)
        elif "pcl" in kind:
            low = rng.randint(1, 5)
        else:
            low = rng.getrandbits(8)
        out.append((n, m, low))
    return out


def _mode(fmt):
    if "@(r0," in fmt:
        return "half"
    return "ptr" if "@" in fmt.replace("@T", "") else "any"


def optest(seed=1):
    """(code image, [(entry, label, mode)]) of the instruction test."""
    rng = random.Random(seed)
    code = bytearray()
    funcs = []
    pc = OPTEST_BASE
    # one instruction, alone and in the delay slot of rts: 32-byte blocks
    for fmt in sh2.formats():
        op = sh2._BY_FMT[fmt][2]
        if op in SKIP or fmt in ("lds Rm,pr", "lds.l @Rm+,pr"):
            continue
        for n, m, low in _variants(fmt, rng):
            w = sh2.encode(fmt, n, m, low)
            text = sh2.decode(w, pc).text
            for slot in (False, True):
                block = [0x000B, w, 0x0009] if slot else [w, 0x000B, 0x0009]
                block += [_data_word(rng) for _ in range(16 - len(block))]
                funcs.append((pc, ("rts ; " if slot else "") + text, _mode(fmt)))
                code += b"".join(x.to_bytes(2, "big") for x in block)
                pc += 32
    # control flow
    a = Asm(pc)
    seqs = []

    def seq(label, mode):
        a.align()
        seqs.append((a.here(), label, mode))

    for br, slot in (("bt", None), ("bf", None), ("bt/s", "sett"), ("bf/s", "clrt"), ("bt/s", None)):
        seq("%s, slot %s" % (br, slot or "add"), "any")
        a.op("%s @T" % br, to="L%d" % len(seqs))
        if slot:
            a.op(slot)
        elif br.endswith("/s"):
            a.op("add #imm,Rn", n=2, low=4)
        a.op("add #imm,Rn", n=1, low=1)
        a.label("L%d" % len(seqs))
        a.op("movt Rn", n=3)
        a.op("rts")
        a.op("nop")
    seq("bra into its own slot", "any")
    a.op("bra @T", to="S%d" % len(seqs))
    a.label("S%d" % len(seqs))
    a.op("add #imm,Rn", n=1, low=1)
    a.op("rts")
    a.op("nop")
    seq("bra over", "any")
    a.op("bra @T", to="B%d" % len(seqs))
    a.op("add #imm,Rn", n=2, low=1)
    a.op("add #imm,Rn", n=2, low=5)
    a.label("B%d" % len(seqs))
    a.op("rts")
    a.op("nop")
    seq("dt loop", "loop")
    a.label("D%d" % len(seqs))
    a.op("add Rm,Rn", n=3, m=2)
    a.op("dt Rn", n=1)
    a.op("bf @T", to="D%d" % len(seqs))
    a.op("rts")
    a.op("nop")
    seq("bf/s loop", "loop")
    a.label("E%d" % len(seqs))
    a.op("dt Rn", n=1)
    a.op("bf/s @T", to="E%d" % len(seqs))
    a.op("add #imm,Rn", n=3, low=3)
    a.op("rts")
    a.op("nop")
    seq("subroutine", "stack")                          # an entry of its own, called below
    a.label("SUB")
    a.op("add Rm,Rn", n=3, m=1)
    a.op("rts")
    a.op("mov Rm,Rn", n=4, m=3)
    seq("bsr", "stack")
    a.op("sts.l pr,@-Rn", n=15)
    a.op("bsr @T", to="SUB")
    a.op("add #imm,Rn", n=1, low=1)
    a.op("lds.l @Rm+,pr", m=15)
    a.op("rts")
    a.op("add #imm,Rn", n=2, low=2)
    seq("jsr, rn overwritten in the slot", "stack")
    a.op("sts.l pr,@-Rn", n=15)
    a.op("mov.l @T,Rn", n=5, to="J%d" % len(seqs))
    a.op("jsr @Rn", n=5)
    a.op("mov #imm,Rn", n=5, low=0)
    a.op("lds.l @Rm+,pr", m=15)
    a.op("rts")
    a.op("nop")
    a.align()
    a.label("J%d" % len(seqs))
    a.long("SUB")
    seq("bsrf", "stack")
    k = len(seqs)
    a.op("sts.l pr,@-Rn", n=15)
    a.op("mov.l @T,Rn", n=6, to="F%d" % k)
    a.label("BSRF%d" % k)
    a.op("bsrf Rn", n=6)
    a.op("nop")
    a.op("lds.l @Rm+,pr", m=15)
    a.op("rts")
    a.op("nop")
    a.align()
    a.label("F%d" % k)
    a.long((a.labels["SUB"] - (a.labels["BSRF%d" % k] + 4)) & M32)
    seq("tail jmp", "stack")
    a.op("mov.l @T,Rn", n=5, to="T%d" % len(seqs))
    a.op("jmp @Rn", n=5)
    a.op("add #imm,Rn", n=2, low=7)
    a.align()
    a.label("T%d" % len(seqs))
    a.long("SUB")
    seq("rts reads pr before its slot", "stack")
    a.op("mov.l @T,Rn", n=0, to="P%d" % len(seqs))
    a.op("mov.l Rm,@-Rn", n=15, m=0)
    a.op("rts")
    a.op("lds.l @Rm+,pr", m=15)
    a.align()
    a.label("P%d" % len(seqs))
    a.long(0x12345678)
    seq("switch (braf)", "switch")
    k = len(seqs)
    a.op("mov #imm,Rn", n=1, low=3)
    a.op("cmp/hs Rm,Rn", n=0, m=1)
    a.op("bt @T", to="DEF%d" % k)
    a.op("shll Rn", n=0)
    a.op("mov Rm,Rn", n=1, m=0)
    a.op("mova @T,r0", to="TAB%d" % k)
    a.op("mov.w @(r0,Rm),Rn", n=0, m=1)
    a.op("braf Rn", n=0)
    a.op("nop")
    a.label("BASE%d" % k)
    a.align()
    a.label("TAB%d" % k)
    for i in range(3):
        a.word(lambda L, i=i: L["C%d_%d" % (k, i)] - L["BASE%d" % k])
    for i in range(3):
        a.label("C%d_%d" % (k, i))
        a.op("rts")
        a.op("mov #imm,Rn", n=2, low=10 + i)
    a.label("DEF%d" % k)
    a.op("rts")
    a.op("mov #imm,Rn", n=2, low=13)
    for d0, name in (("div0u", "div0u"), ("div0s Rm,Rn", "div0s")):
        seq("32 division steps after %s" % name, "any")
        a.op(d0, n=2, m=0)
        for _ in range(32):
            a.op("rotcl Rn", n=1)
            a.op("div1 Rm,Rn", n=2, m=0)
        a.op("rotcl Rn", n=1)
        a.op("rts")
        a.op("nop")
    seq("mac.w x4, S as given", "ptr")
    a.op("clrmac")
    for _ in range(4):
        a.op("mac.w @Rm+,@Rn+", n=4, m=5)
    a.op("sts macl,Rn", n=0)
    a.op("rts")
    a.op("sts mach,Rn", n=1)
    code += a.assemble()
    funcs += seqs
    return bytes(code), funcs


def optest_blob(seed=1):
    rng = random.Random(seed * 7919)
    return bytes(rng.getrandbits(8) for _ in range(BLOB_SIZE))


def write_optest(out_dir, seed=1, vectors=12):
    """Write optest.bin, optest_blob.bin and optest.txt (the vectors) into
    out_dir; returns (path of the code image, its base, its entries)."""
    os.makedirs(out_dir, exist_ok=True)
    code, funcs = optest(seed)
    blob = optest_blob(seed)
    code_path = os.path.abspath(os.path.join(out_dir, "optest.bin")).replace("\\", "/")
    blob_path = os.path.abspath(os.path.join(out_dir, "optest_blob.bin")).replace("\\", "/")
    for path, data in ((code_path, code), (blob_path, blob)):
        with open(path, "wb") as f:
            f.write(data)
    rng = random.Random(seed + 1)
    mem = new_memory([(code, OPTEST_BASE), (blob, BLOB)])
    lines = ["# saturn-recomp instruction test: %d functions" % len(funcs),
             "image %08X %s" % (OPTEST_BASE, code_path),
             "image %08X %s" % (BLOB, blob_path),
             "module OPTEST"]
    for entry, label, mode in funcs:
        ls, _ = record(mem, entry, label, [rand_state(rng, mode) for _ in range(vectors)], max_steps=10_000)
        lines += ls
    with open(os.path.join(out_dir, "optest.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return code_path, OPTEST_BASE, [e for e, _, _ in funcs]


def load_names(path):
    names = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                parts = line.rstrip("\n").split("\t")
                names[int(parts[0], 16)] = parts[1]
    return names


def main(argv=None):
    from .__main__ import parse_spec
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--optest", metavar="DIR", help="write the instruction test into DIR")
    ap.add_argument("--image", action="append", default=[], help="NAME=FILE@BASE[+SEED,...]")
    ap.add_argument("--test", help="the module whose functions are tested")
    ap.add_argument("--funcs", default="", help="entries to test (hex, comma-separated)")
    ap.add_argument("--auto", action="store_true", help="also every function that runs on its stack alone")
    ap.add_argument("--names", help="TSV of names (address, name) for the labels")
    ap.add_argument("--vectors", type=int, default=16)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    if a.optest:
        path, base, entries = write_optest(a.optest, a.seed)
        print("%d functions at %08X: %s" % (len(entries), base, path))
        return
    specs = [parse_spec(s) for s in a.image]
    images = [(open(p, "rb").read(), b) for _, p, b, _ in specs]
    name, path, base, seeds = next(s for s in specs if s[0] == a.test)
    prog = discover.Program(sh2.Image(open(path, "rb").read(), base), [base] + seeds)
    rng = random.Random(a.seed)
    scratch = os.path.abspath(os.path.splitext(a.out)[0] + ".scratch.bin").replace("\\", "/")
    data = scratch_data(a.seed)
    with open(scratch, "wb") as f:
        f.write(data)
    images.append((data, SCRATCH))
    funcs = {int(x, 16): "game" for x in a.funcs.split(",") if x}
    if a.auto:
        found = pure_functions(images, prog, rng)
        n_args = sum(1 for m in found.values() if m == "args")
        print("%s: %d of %d functions run on their stack alone, %d more with pointer arguments"
              % (name, len(found) - n_args, len(prog.funcs), n_args))
        for e, mode in found.items():
            funcs.setdefault(e, mode)
    names = load_names(a.names) if a.names else {}
    lines = ["# saturn-recomp self-test: %s, %d functions" % (name, len(funcs))]
    for (n, p, b, _) in specs:
        lines.append("image %08X %s" % (b, os.path.abspath(p).replace("\\", "/")))
    lines.append("image %08X %s" % (SCRATCH, scratch))
    for (n, _, _, _) in specs:
        lines.append("module %s" % n)
    mem = new_memory(images)
    total = 0
    for e, mode in funcs.items():
        if e not in prog.funcs:
            print("  %08X is not an entry of %s: skipped" % (e, name))
            continue
        ls, k = record(mem, e, names.get(e, "%s_%08X" % (name.lower(), e)) + " (%s)" % mode,
                       [rand_state(rng, mode) for _ in range(a.vectors)])
        lines += ls
        total += k
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d functions, %d vectors -> %s" % (name, len(funcs), total, a.out))


if __name__ == "__main__":
    main()
