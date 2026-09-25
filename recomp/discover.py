"""Function discovery for flat SH-2 programs without symbols.

    python -m saturnkit.recomp.discover FILE.BIN --base 0600B000 [--seeds a,b,..] [--report]

On the SH-2, code and data share the text: every function is followed by
its literal pool, switch tables sit right after the jump that reads them,
and nothing says where one ends. So this is a recursive descent that
classifies each halfword as code or data while it finds the functions,
written for Hitachi SHC's output (and GCC's, which is close):

1. Seeds: the entry point, the caller's list, and every call target found
   on the way: `bsr` targets, and `jsr @rn` whose rn was loaded from the
   literal pool in the same straight line (`mov.l @(disp,PC),rn`).
2. A function is the code reachable from its entry: conditional branches
   (both ways), `bra`, and `jmp @rn` with a literal target (SHC's branch
   for distances beyond `bra`'s 4 KiB), switch tables, fall-through. A
   branch to another function's entry is a tail call and is not followed.
   Functions may share code (a tail into the middle of another is followed
   as its own); the recompiler duplicates it, which is harmless.
3. Every delayed branch runs its slot: the slot is code, its literal is
   data.
4. Literal loads mark their 2 or 4 bytes as data; `mova` marks nothing
   until a switch claims the table.
5. SHC switch: `mov #N,r1; cmp/hs r1,r0; bt default; shll|shll2 r0;
   mov r0,r1; mova TABLE,r0; mov.w|mov.l @(r0,r1),r0; braf r0`. N entries
   of 16 or 32 bits, each an offset from the `braf` + 4. The runtime
   library's form: `mov #N,rB; cmp/hs r0,rB; bf default; mov.l TABLE,rT;
   mov.l @(r0,rT),rJ; jmp @rJ`, r0 a byte offset, N/4 + 1 absolute entries;
   and its shift ladders, a table of signed bytes added to a base.
6. Unreached code: after the pass, a literal value that points into the
   program at a *boundary* (after data, or after a terminator's delay
   slot) and is not data is a function reached by pointer (a callback, an
   interrupt handler, a slave job). It becomes a seed if its descent meets
   no undefined opcode and no data. Failing that, unclassified code that
   starts with a stack-frame prologue at a boundary (SHC links whole
   object files, so uncalled functions sit between called ones) is taken
   the same way. Repeat to a fixed point.
7. Registers that hold call targets across a function (SHC keeps a
   often-called address in r8-r14) are found by forward constant
   propagation over the function's code; calls clobber r0-r7.

`Program` keeps: funcs {entry: Function}, code (halfword addresses), data
(halfword addresses), switches {braf address: [targets]}, and the
indirect jumps and calls it could not resolve.
"""
import argparse
import bisect
import collections

from .. import sh2

TERMINATORS = {"rts", "rte", "bra", "jmp", "braf"}


def _writes(ins):
    """Registers an instruction writes (for the literal look-back)."""
    f = ins.fmt
    out = set()
    if f.endswith("Rn") and ins.n is not None:
        out.add(ins.n)
    elif f.endswith(",r0") or ins.op == "mova":
        out.add(0)
    elif ins.op in ("movt", "dt") or (f.split()[0] == ins.op and f.endswith("Rn")):
        out.add(ins.n)
    if "@Rm+" in f and ins.m is not None:
        out.add(ins.m)
    if "@-Rn" in f and ins.n is not None:
        out.add(ins.n)
    return out


def _transfer(img, ins, st):
    """Register constants after `ins`, from `st` (a list of 16 values or None)."""
    op, f = ins.op, ins.fmt
    if op in ("jsr", "bsr", "bsrf"):
        for r in range(8):              # SHC: r0-r7 caller-saved
            st[r] = None
        return
    if ins.target is not None and op in ("mov.l", "mov.w") and ins.size:
        st[ins.n] = img.literal(ins)
        return
    if op == "mova":
        st[0] = ins.target
        return
    if op == "mov" and ins.imm is not None:
        st[ins.n] = ins.imm & 0xFFFFFFFF
        return
    if op == "mov" and f == "mov Rm,Rn":
        st[ins.n] = st[ins.m]
        return
    if op == "add" and ins.imm is not None:
        v = st[ins.n]
        st[ins.n] = None if v is None else (v + ins.imm) & 0xFFFFFFFF
        return
    for r in _writes(ins):
        st[r] = None


class Function:
    __slots__ = ("entry", "code", "calls", "tails", "unresolved", "bad", "tables")

    def __init__(self, entry):
        self.entry = entry
        self.code = set()          # instruction addresses reached
        self.calls = set()         # call targets (bsr, jsr with literal)
        self.tails = set()         # tail calls to other entries
        self.unresolved = []       # (addr, what) indirect jumps not resolved
        self.bad = None            # first problem met, or None
        self.tables = set()        # halfwords of the switch tables it reads


class Program:
    def __init__(self, img, seeds=(), bounds=None):
        self.img = img
        self.lo, self.hi = bounds or (img.base, img.end)
        self.funcs = {}
        self.code = set()
        self.data = set()
        self.switches = {}
        self.tables = set()        # halfwords of switch tables (not function pointers)
        self.ext_calls = collections.Counter()   # calls through pointers: loaded-from address
        self._pending = list(seeds) or [img.base]
        self._rejected = set()
        self.run()

    # -- helpers
    def inside(self, a):
        return self.lo <= a < self.hi and not a & 1

    def _mark_data(self, a, size):
        for k in range(0, size, 2):
            self.data.add(a + k)

    def _literal_for(self, at, reg, limit=24):
        """Value loaded into `reg` by a literal load in the straight line before `at`.

        Returns ("lit", value), ("ptr", address) for `mov.l @rX,reg` with rX a
        literal (a call through a pointer, e.g. a BIOS vector), or None.
        """
        a = at - 2
        want = reg
        for _ in range(limit):
            if not self.inside(a) or a in self.data:
                return None
            ins = self.img.insn(a)
            if ins.op in ("bt", "bf", "bt/s", "bf/s", "bra", "bsr", "jmp", "jsr", "rts", "rte", "braf", "bsrf"):
                return None
            if want in _writes(ins):
                if ins.op == "mov.l" and ins.size == 4 and ins.target is not None:
                    v = self.img.literal(ins)
                    return ("lit", v) if v is not None else None
                if ins.op == "mov.l" and ins.fmt in ("mov.l @Rm,Rn", "mov.l @(disp,Rm),Rn"):
                    inner = self._literal_for(a, ins.m, limit)
                    if inner and inner[0] == "lit" and ins.fmt == "mov.l @Rm,Rn":
                        return ("ptr", inner[1])
                    if inner:                      # a table reached through a pointer
                        return ("ptr", inner[1])
                    return None
                if ins.op == "mov" and ins.fmt == "mov Rm,Rn":
                    want = ins.m
                    a -= 2
                    continue
                return None
            a -= 2
        return None

    def _abs_switch(self, jmp):
        """SHC's absolute jump table: mov #N,rB; cmp/hs r0,rB; bf default;
        mov.l @(disp,PC),rT; mov.l @(r0,rT),rJ; jmp @rJ  (r0 a byte offset <= N)."""
        img = self.img
        j = img.insn(jmp)
        ld = img.insn(jmp - 2)
        if ld.op != "mov.l" or ld.fmt != "mov.l @(r0,Rm),Rn" or ld.n != j.n:
            return None
        lit = self._literal_for(jmp - 2, ld.m)
        if not lit or lit[0] != "lit":
            return None
        table = lit[1]
        n = None
        a = jmp - 4
        for _ in range(8):
            ins = img.insn(a)
            if ins.op == "cmp/hs" and ins.m == 0:
                nxt = img.insn(a + 2)
                for b in range(a - 2, a - 12, -2):
                    k = img.insn(b)
                    if k.op == "mov" and k.imm is not None and k.n == ins.n:
                        n = k.imm // 4 + 1 if nxt.op in ("bf", "bf/s") else None
                        break
                break
            a -= 2
        if not n or n > 1024 or not img.contains(table, 4 * n):
            return None
        return [img.u32(table + 4 * k) for k in range(n)], table, 4 * n

    def _byte_switch(self, jmp):
        """The runtime library's shift ladders: mov.l TAB,rT; add ri,rT;
        mov.b @rT,rT; mov.l BASE,rJ; add rT,rJ; jmp @rJ, bounded by
        mov #N,rN; cmp/ge rN,ri; bt (index < N). Targets: BASE + signed byte."""
        img = self.img
        seq = [img.insn(jmp - 2 * k) for k in range(5, 0, -1)]
        j = img.insn(jmp)
        t1, add1, ldb, t2, add2 = seq
        if not (t1.op == "mov.l" and t1.size == 4 and add1.op == "add" and add1.fmt == "add Rm,Rn"
                and add1.n == t1.n and ldb.fmt == "mov.b @Rm,Rn" and ldb.m == t1.n
                and t2.op == "mov.l" and t2.size == 4 and t2.n == j.n
                and add2.fmt == "add Rm,Rn" and add2.n == j.n and add2.m == ldb.n):
            return None
        tab, base = img.literal(t1), img.literal(t2)
        idx = add1.m
        n = None
        for a in range(jmp - 12, jmp - 40, -2):
            ins = img.insn(a)
            if ins.op == "cmp/ge" and ins.n == idx:
                for b in range(a - 2, a - 12, -2):
                    k = img.insn(b)
                    if k.op == "mov" and k.imm is not None and k.n == ins.m:
                        n = k.imm
                        break
                break
        if tab is None or base is None or not n or n > 256 or not img.contains(tab, n):
            return None
        targets = []
        for k in range(n):
            v = img.data[tab - img.base + k]
            targets.append((base + (v - 256 if v & 0x80 else v)) & 0xFFFFFFFF)
        return targets, tab, (n + 1) & ~1

    def _switch(self, braf):
        """Targets of an SHC switch ending at `braf`, and its table, or None."""
        img = self.img
        ld = img.insn(braf - 2)
        mova = img.insn(braf - 4)
        if mova.op != "mova" or ld.op not in ("mov.w", "mov.l") or "@(r0," not in ld.fmt:
            return None
        size = 2 if ld.op == "mov.w" else 4
        table = mova.target
        n = None
        # the bound: mov #N,rX ; cmp/hs rX,r0 ; bt default  (within a few instructions)
        a = braf - 6
        for _ in range(8):
            ins = img.insn(a)
            if ins.op == "cmp/hs" and ins.n == 0:
                for b in range(a - 2, a - 12, -2):
                    j = img.insn(b)
                    if j.op == "mov" and j.imm is not None and j.n == ins.m:
                        n = j.imm
                        break
                break
            a -= 2
        if not n or n <= 0 or n > 1024:
            return None
        base = braf + 4
        targets = []
        for k in range(n):
            ea = table + k * size
            if not img.contains(ea, size):
                return None
            v = img.u16(ea) if size == 2 else img.u32(ea)
            if size == 2 and v & 0x8000:
                v -= 0x10000
            if size == 4 and v & 0x80000000:
                v -= 1 << 32
            targets.append((base + v) & 0xFFFFFFFF)
        return targets, table, n * size

    def _constants(self, code, entry, wanted, switches=None):
        """Register constants at each address in `wanted`, by forward dataflow over `code`."""
        img = self.img
        state = {entry: [None] * 16}
        work = [entry]
        out = {}
        while work:
            a = work.pop()
            st = list(state[a])
            ins = img.insn(a)
            if a in wanted:
                out[a] = {r: st[r] for r in range(16)}
            _transfer(img, ins, st)
            succ = []
            if ins.delay:
                slot = img.insn(a + 2)
                if ins.op in ("jsr", "bsr", "bsrf"):
                    # the slot runs before the call clobbers
                    st2 = list(state[a])
                    _transfer(img, slot, st2)
                    _transfer(img, ins, st2)
                    st = st2
                    succ = [a + 4]
                else:
                    _transfer(img, slot, st)
                    if ins.op in ("bt/s", "bf/s"):
                        succ = [ins.target, a + 4]
                    elif ins.op == "bra":
                        succ = [ins.target]
            elif ins.op in ("bt", "bf"):
                succ = [ins.target, a + 2]
            elif ins.op not in ("rts", "rte", "jmp", "braf"):
                succ = [a + 2]
            if ins.op in ("braf", "jmp"):
                succ = (switches or {}).get(a) or self.switches.get(a, [])
            for t in succ:
                if t not in code:
                    continue
                old = state.get(t)
                if old is None:
                    state[t] = st
                    work.append(t)
                else:
                    merged = [x if x == y else None for x, y in zip(old, st)]
                    if merged != old:
                        state[t] = merged
                        work.append(t)
        return out

    # -- descent
    def _descend(self, entry, commit=True):
        f = Function(entry)
        data, switches = set(), {}
        work, pending = [entry], []
        while True:
            while work:
                self._walk(f, work.pop(), data, switches, work, pending)
            if not pending:
                break
            # indirect calls and jumps left: constants over the function so far
            consts = self._constants(f.code, entry, [at for at, _ in pending], switches)
            for at, what in pending:
                v = consts.get(at, {}).get(self.img.insn(at).n)
                if v is None:
                    f.unresolved.append((at, what))
                elif what == "jsr":
                    f.calls.add(v)
                elif (v in self.funcs and v != entry) or not self.inside(v):
                    f.tails.add(v)
                else:
                    work.append(v)
            pending = []
            if not work:
                break
        # code that the descent itself later marked as data is a contradiction
        clash = f.code & data
        if clash and not f.bad:
            f.bad = ("code is data", min(clash))
        if commit:
            self.funcs[entry] = f
            self.code |= f.code
            self.data |= data
            self.switches.update(switches)
            self.tables |= f.tables
        return f, data

    def _code_literal(self, ins, data):
        if ins.target is not None and ins.op in ("mov.w", "mov.l", "mova") and ins.size:
            for k in range(0, ins.size, 2):
                data.add(ins.target + k)

    def _walk(self, f, a, data, switches, work, pending):
        """Follow straight-line code from `a` until a terminator or known code."""
        img = self.img
        while True:
            if a in f.code:
                return
            if not self.inside(a):
                f.bad = f.bad or ("outside", a)
                return
            if a in self.data or a in data:
                f.bad = f.bad or ("into data", a)
                return
            ins = img.insn(a)
            if ins.op == ".word":
                f.bad = f.bad or ("undefined", a)
                return
            f.code.add(a)
            self._code_literal(ins, data)
            op = ins.op
            if ins.delay:
                slot = a + 2
                if self.inside(slot) and slot not in self.data:
                    s = img.insn(slot)
                    if s.op == ".word" or s.delay:
                        f.bad = f.bad or ("bad slot", slot)
                    else:
                        f.code.add(slot)
                        self._code_literal(s, data)
            if op in ("bt", "bf"):
                work.append(ins.target)
                a += 2
            elif op in ("bt/s", "bf/s"):
                work.append(ins.target)
                a += 4
            elif op == "bsr":
                f.calls.add(ins.target)
                a += 4
            elif op == "bsrf":
                f.unresolved.append((a, "bsrf"))
                a += 4
            elif op == "jsr":
                r = self._literal_for(a, ins.n)
                if r and r[0] == "lit":
                    f.calls.add(r[1])
                elif r and r[0] == "ptr":
                    self.ext_calls[r[1]] += 1
                else:
                    pending.append((a, "jsr"))
                a += 4
            elif op == "bra":
                t = ins.target
                if t in self.funcs and t != f.entry:
                    f.tails.add(t)
                else:
                    work.append(t)
                return
            elif op == "jmp":
                r = self._literal_for(a, ins.n)
                if r and r[0] == "lit":
                    t = r[1]
                    if (t in self.funcs and t != f.entry) or not self.inside(t):
                        f.tails.add(t)
                    else:
                        work.append(t)
                elif r and r[0] == "ptr":
                    self.ext_calls[r[1]] += 1
                else:
                    sw = self._abs_switch(a) or self._byte_switch(a)
                    if sw:
                        targets, table, size = sw
                        switches[a] = targets
                        for k in range(0, size, 2):
                            data.add(table + k)
                            f.tables.add(table + k)
                        work.extend(targets)
                    else:
                        pending.append((a, "jmp"))
                return
            elif op == "braf":
                sw = self._switch(a)
                if sw:
                    targets, table, size = sw
                    switches[a] = targets
                    for k in range(0, size, 2):
                        data.add(table + k)
                        f.tables.add(table + k)
                    work.extend(targets)
                else:
                    f.unresolved.append((a, "braf"))
                return
            elif op in ("rts", "rte"):
                return
            else:
                a += 2

    def run(self):
        while True:
            while self._pending:
                e = self._pending.pop()
                if e in self.funcs or not self.inside(e):
                    continue
                f, _ = self._descend(e)
                for t in sorted(f.calls | f.tails):
                    if t not in self.funcs and self.inside(t):
                        self._pending.append(t)
            added = self._pointer_seeds()
            if not added:
                added = self._prologue_seeds()
            if not added:
                added = self._address_taken()
            if not added:
                break
        # branches into another function's entry found later are tail calls:
        # harmless for the recompiler (duplicated code), counted for the report
        self.shared = sum(1 for f in self.funcs.values()
                          for e in self.funcs if e != f.entry and e in f.code)

    def _boundary(self, a):
        p = a - 2
        if p in self.data:
            return True
        q = a - 4
        if q in self.code:
            i = self.img.insn(q)
            return i.op in TERMINATORS and i.op != "braf"
        return p < self.lo or not self.img.contains(p)

    def _pointer_seeds(self):
        cands = set()
        words = [a for a in self.data if not a & 3 and a + 2 in self.data]
        # and the unclassified words: pointer tables in the data section
        for start, n in self.gaps():
            words.extend(range((start + 3) & ~3, start + n - 3, 4))
        for a in sorted(words):
            if not self.img.contains(a, 4):
                continue
            v = self.img.u32(a)
            if self.inside(v) and v not in self.code and v not in self.data and v not in self._rejected:
                cands.add(v)
        added = 0
        for v in sorted(cands):
            if not self._boundary(v):
                continue                # may become one once its neighbour is found
            f, data = self._descend(v, commit=False)
            if f.bad or f.code & self.data or data & self.code:
                self._rejected.add(v)
                continue
            self._descend(v)
            for t in f.calls | f.tails:
                if t not in self.funcs and self.inside(t):
                    self._pending.append(t)
            added += 1
        return added

    def _address_taken(self):
        """Code addresses that appear as 32-bit literals become entries of their own.

        They are function pointers (callbacks, jump targets reached through a
        register) or tail calls made with `jmp`; followed so far as part of
        the function that reached them, they now also get their own entry.
        """
        added = 0
        for a in sorted(self.data - self.tables):
            if a & 3 or a + 2 not in self.data:
                continue
            v = self.img.u32(a)
            if v in self.code and v not in self.funcs and v not in self._rejected:
                f, data = self._descend(v, commit=False)
                if f.bad:
                    self._rejected.add(v)
                    continue
                self._descend(v)
                added += 1
        return added

    @staticmethod
    def _is_prologue(w):
        # mov.l rN,@-r15 (N = 8..14) ; sts.l pr,@-r15 ; add #-N,r15
        return (w & 0xFF0F) == 0x2F06 and 8 <= (w >> 4) & 15 <= 14 or w == 0x4F22             or (w & 0xFF80) == 0x7F80

    def _prologue_seeds(self):
        """Unreached code that starts with a stack-frame prologue at a boundary:
        the start of a gap (after padding), or anywhere in it right after a
        terminator and its delay slot."""
        added = 0
        img = self.img
        for start, n in self.gaps():
            end = start + n
            a = start
            while a < end and img.u16(a) in (0x0009, 0x0000):
                a += 2
            cands = [a] if a < end and self._boundary(a) else []
            for p in range(start + 4, end, 2):
                if sh2.decode(img.u16(p - 4), p - 4).op in ("rts", "rte", "bra", "jmp"):
                    q = p
                    while q < end and img.u16(q) in (0x0009, 0x0000):
                        q += 2
                    cands.append(q)
            for c in cands:
                if c >= end or c in self._rejected or c in self.code or c in self.funcs:
                    continue
                if not self._is_prologue(img.u16(c)):
                    continue
                f, data = self._descend(c, commit=False)
                if f.bad or f.code & self.data or data & self.code:
                    self._rejected.add(c)
                    continue
                self._descend(c)
                for t in f.calls | f.tails:
                    if t not in self.funcs and self.inside(t):
                        self._pending.append(t)
                added += 1
        return added

    # -- report
    def gaps(self):
        """Runs of halfwords that are neither code nor data: (start, length)."""
        out, start = [], None
        for a in range(self.lo, self.hi, 2):
            known = a in self.code or a in self.data
            if not known and start is None:
                start = a
            elif known and start is not None:
                out.append((start, a - start))
                start = None
        if start is not None:
            out.append((start, self.hi - start))
        return out

    def summary(self):
        bad = [f for f in self.funcs.values() if f.bad]
        unres = [u for f in self.funcs.values() for u in f.unresolved]
        gaps = self.gaps()
        gap_bytes = sum(n for _, n in gaps)
        return {
            "functions": len(self.funcs),
            "code bytes": len(self.code) * 2,
            "data bytes": len(self.data) * 2,
            "unclassified bytes": gap_bytes,
            "switch tables": len(self.switches),
            "unresolved indirect": len(unres),
            "functions with problems": len(bad),
            "entries inside other functions": self.shared,
            "calls through pointers": sum(self.ext_calls.values()),
        }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file")
    ap.add_argument("--base", default="0600B000")
    ap.add_argument("--seeds", default="", help="extra entry points, hex, comma-separated")
    ap.add_argument("--end", help="end of text (hex), default the end of the file")
    ap.add_argument("--report", action="store_true", help="list problems, unresolved jumps, gaps")
    ap.add_argument("--out", help="write the function list (entry, size of reached code) as TSV")
    a = ap.parse_args(argv)
    data = open(a.file, "rb").read()
    base = int(a.base, 16)
    img = sh2.Image(data, base)
    seeds = [base] + [int(s, 16) for s in a.seeds.split(",") if s]
    bounds = (base, int(a.end, 16)) if a.end else None
    p = Program(img, seeds, bounds)
    for k, v in p.summary().items():
        print("%-32s %d" % (k, v))
    if a.report:
        print("\nfunctions with problems:")
        for f in sorted(p.funcs.values(), key=lambda f: f.entry):
            if f.bad:
                print("  %08X  %s at %08X" % (f.entry, f.bad[0], f.bad[1]))
        print("\nunresolved indirect jumps:")
        for f in sorted(p.funcs.values(), key=lambda f: f.entry):
            for at, what in f.unresolved:
                print("  %08X  %s (in %08X)" % (at, what, f.entry))
        print("\ncalls through pointers (pointer address: count):")
        for ptr, n in p.ext_calls.most_common():
            from .. import hw
            print("  %08X  %4d  %s" % (ptr, n, hw.name(ptr) or ""))
        print("\nlargest unclassified runs:")
        for s, n in sorted(p.gaps(), key=lambda g: -g[1])[:25]:
            print("  %08X  %6d bytes" % (s, n))
    if a.out:
        with open(a.out, "w") as fo:
            for e in sorted(p.funcs):
                fo.write("%08X\t%d\n" % (e, len(p.funcs[e].code) * 2))


if __name__ == "__main__":
    main()
