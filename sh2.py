"""SH-2 (SH7604) decoding, disassembly and cross-references.

    python -m saturnkit.sh2 FILE.BIN --base 0600B000 --at 0600B000 [--count 64]
    python -m saturnkit.sh2 FILE.BIN --base 0600B000 --census
    python -m saturnkit.sh2 FILE.BIN --base 0600B000 --refs 25C00000[:25D00000]
    python -m saturnkit.sh2 FILE.BIN --find-base            # where does it load?

Every instruction is 16 bits, big-endian. Branches with a delay slot
(bra, bsr, jmp, jsr, rts, rte, braf, bsrf, bt/s, bf/s) run the next
instruction before the jump lands. PC-relative loads take their literal
from the pool: mov.w @(disp,PC) reads at PC+4+disp*2, mov.l at
(PC & ~3)+4+disp*4, mova the same address. Those literal loads are how
code builds 32-bit constants, so they are also where addresses (hardware
registers, data, function pointers) are found: `refs` lists them.

decode(word, pc) returns an Insn with .op (canonical mnemonic), .n, .m,
.imm, .disp, .target (resolved branch target or literal address),
.delay (True for delayed branches), .text.
"""
import argparse
import collections
import struct

from . import hw


class Insn:
    __slots__ = ("pc", "word", "op", "fmt", "n", "m", "imm", "disp", "target", "delay", "size")

    def __init__(self, pc, word, op, fmt, n=None, m=None, imm=None, disp=None,
                 target=None, delay=False, size=None):
        self.pc, self.word, self.op, self.fmt = pc, word, op, fmt
        self.n, self.m, self.imm, self.disp = n, m, imm, disp
        self.target, self.delay, self.size = target, delay, size

    @property
    def text(self):
        f = self.fmt
        rep = {"Rn": "r%d" % self.n if self.n is not None else "",
               "Rm": "r%d" % self.m if self.m is not None else ""}
        if self.imm is not None:
            rep["#imm"] = "#%d" % self.imm if -256 < self.imm < 256 else "#0x%X" % (self.imm & 0xFFFFFFFF)
        if self.target is not None:
            rep["@T"] = "0x%08X" % self.target
        if self.disp is not None:
            rep["disp"] = "%d" % self.disp
        for k in ("Rn", "Rm", "#imm", "@T", "disp"):
            if k in rep:
                f = f.replace(k, rep[k])
        return f

    def __repr__(self):
        return "%08X  %04X  %s" % (self.pc, self.word, self.text)


def _s8(x):
    return x - 0x100 if x & 0x80 else x


def _s12(x):
    return x - 0x1000 if x & 0x800 else x


# (mask, value, op, format, kind) ; kind tells how to pull fields
#   n/m: registers at bits 8-11 / 4-7; d4, d8, d12, i8: low fields
_TABLE = []


def _t(pattern, op, fmt, kind=""):
    mask = val = 0
    for ch in pattern:
        mask <<= 1
        val <<= 1
        if ch in "01":
            mask |= 1
            val |= ch == "1"
    nsh = 15 - pattern.rindex("n") if "n" in pattern else None
    msh = 15 - pattern.rindex("m") if "m" in pattern else None
    _TABLE.append((mask, val, op, fmt, kind, nsh, msh))


# fixed
_t("0000000000001001", "nop", "nop")
_t("0000000000001011", "rts", "rts", "delay")
_t("0000000000101011", "rte", "rte", "delay")
_t("0000000000001000", "clrt", "clrt")
_t("0000000000011000", "sett", "sett")
_t("0000000000101000", "clrmac", "clrmac")
_t("0000000000011001", "div0u", "div0u")
_t("0000000000011011", "sleep", "sleep")
# n-only
_t("0000nnnn00101001", "movt", "movt Rn", "n")
_t("0100nnnn00010001", "cmp/pz", "cmp/pz Rn", "n")
_t("0100nnnn00010101", "cmp/pl", "cmp/pl Rn", "n")
_t("0100nnnn00010000", "dt", "dt Rn", "n")
_t("0100nnnn00000100", "rotl", "rotl Rn", "n")
_t("0100nnnn00000101", "rotr", "rotr Rn", "n")
_t("0100nnnn00100100", "rotcl", "rotcl Rn", "n")
_t("0100nnnn00100101", "rotcr", "rotcr Rn", "n")
_t("0100nnnn00100000", "shal", "shal Rn", "n")
_t("0100nnnn00100001", "shar", "shar Rn", "n")
_t("0100nnnn00000000", "shll", "shll Rn", "n")
_t("0100nnnn00000001", "shlr", "shlr Rn", "n")
_t("0100nnnn00001000", "shll2", "shll2 Rn", "n")
_t("0100nnnn00001001", "shlr2", "shlr2 Rn", "n")
_t("0100nnnn00011000", "shll8", "shll8 Rn", "n")
_t("0100nnnn00011001", "shlr8", "shlr8 Rn", "n")
_t("0100nnnn00101000", "shll16", "shll16 Rn", "n")
_t("0100nnnn00101001", "shlr16", "shlr16 Rn", "n")
_t("0100nnnn00011011", "tas.b", "tas.b @Rn", "n")
_t("0100nnnn00101011", "jmp", "jmp @Rn", "n delay")
_t("0100nnnn00001011", "jsr", "jsr @Rn", "n delay")
_t("0000nnnn00100011", "braf", "braf Rn", "n delay")
_t("0000nnnn00000011", "bsrf", "bsrf Rn", "n delay")
# control / system registers
for reg, code in (("sr", 0), ("gbr", 1), ("vbr", 2)):
    _t("0100mmmm%s1110" % format(code, "04b"), "ldc", "ldc Rm,%s" % reg)
    _t("0100mmmm%s0111" % format(code, "04b"), "ldc.l", "ldc.l @Rm+,%s" % reg)
    _t("0000nnnn%s0010" % format(code, "04b"), "stc", "stc %s,Rn" % reg)
    _t("0100nnnn%s0011" % format(code, "04b"), "stc.l", "stc.l %s,@-Rn" % reg)
for reg, code in (("mach", 0), ("macl", 1), ("pr", 2)):
    _t("0100mmmm%s1010" % format(code, "04b"), "lds", "lds Rm,%s" % reg)
    _t("0100mmmm%s0110" % format(code, "04b"), "lds.l", "lds.l @Rm+,%s" % reg)
    _t("0000nnnn%s1010" % format(code, "04b"), "sts", "sts %s,Rn" % reg)
    _t("0100nnnn%s0010" % format(code, "04b"), "sts.l", "sts.l %s,@-Rn" % reg)
# n, m
_nm = [
    ("0110nnnnmmmm0011", "mov", "mov Rm,Rn"),
    ("0010nnnnmmmm0000", "mov.b", "mov.b Rm,@Rn"),
    ("0010nnnnmmmm0001", "mov.w", "mov.w Rm,@Rn"),
    ("0010nnnnmmmm0010", "mov.l", "mov.l Rm,@Rn"),
    ("0110nnnnmmmm0000", "mov.b", "mov.b @Rm,Rn"),
    ("0110nnnnmmmm0001", "mov.w", "mov.w @Rm,Rn"),
    ("0110nnnnmmmm0010", "mov.l", "mov.l @Rm,Rn"),
    ("0010nnnnmmmm0100", "mov.b", "mov.b Rm,@-Rn"),
    ("0010nnnnmmmm0101", "mov.w", "mov.w Rm,@-Rn"),
    ("0010nnnnmmmm0110", "mov.l", "mov.l Rm,@-Rn"),
    ("0110nnnnmmmm0100", "mov.b", "mov.b @Rm+,Rn"),
    ("0110nnnnmmmm0101", "mov.w", "mov.w @Rm+,Rn"),
    ("0110nnnnmmmm0110", "mov.l", "mov.l @Rm+,Rn"),
    ("0000nnnnmmmm0100", "mov.b", "mov.b Rm,@(r0,Rn)"),
    ("0000nnnnmmmm0101", "mov.w", "mov.w Rm,@(r0,Rn)"),
    ("0000nnnnmmmm0110", "mov.l", "mov.l Rm,@(r0,Rn)"),
    ("0000nnnnmmmm1100", "mov.b", "mov.b @(r0,Rm),Rn"),
    ("0000nnnnmmmm1101", "mov.w", "mov.w @(r0,Rm),Rn"),
    ("0000nnnnmmmm1110", "mov.l", "mov.l @(r0,Rm),Rn"),
    ("0110nnnnmmmm1000", "swap.b", "swap.b Rm,Rn"),
    ("0110nnnnmmmm1001", "swap.w", "swap.w Rm,Rn"),
    ("0010nnnnmmmm1101", "xtrct", "xtrct Rm,Rn"),
    ("0011nnnnmmmm1100", "add", "add Rm,Rn"),
    ("0011nnnnmmmm1110", "addc", "addc Rm,Rn"),
    ("0011nnnnmmmm1111", "addv", "addv Rm,Rn"),
    ("0011nnnnmmmm0000", "cmp/eq", "cmp/eq Rm,Rn"),
    ("0011nnnnmmmm0010", "cmp/hs", "cmp/hs Rm,Rn"),
    ("0011nnnnmmmm0011", "cmp/ge", "cmp/ge Rm,Rn"),
    ("0011nnnnmmmm0110", "cmp/hi", "cmp/hi Rm,Rn"),
    ("0011nnnnmmmm0111", "cmp/gt", "cmp/gt Rm,Rn"),
    ("0010nnnnmmmm1100", "cmp/str", "cmp/str Rm,Rn"),
    ("0011nnnnmmmm0100", "div1", "div1 Rm,Rn"),
    ("0010nnnnmmmm0111", "div0s", "div0s Rm,Rn"),
    ("0011nnnnmmmm1101", "dmuls.l", "dmuls.l Rm,Rn"),
    ("0011nnnnmmmm0101", "dmulu.l", "dmulu.l Rm,Rn"),
    ("0110nnnnmmmm1110", "exts.b", "exts.b Rm,Rn"),
    ("0110nnnnmmmm1111", "exts.w", "exts.w Rm,Rn"),
    ("0110nnnnmmmm1100", "extu.b", "extu.b Rm,Rn"),
    ("0110nnnnmmmm1101", "extu.w", "extu.w Rm,Rn"),
    ("0000nnnnmmmm1111", "mac.l", "mac.l @Rm+,@Rn+"),
    ("0100nnnnmmmm1111", "mac.w", "mac.w @Rm+,@Rn+"),
    ("0000nnnnmmmm0111", "mul.l", "mul.l Rm,Rn"),
    ("0010nnnnmmmm1111", "muls.w", "muls.w Rm,Rn"),
    ("0010nnnnmmmm1110", "mulu.w", "mulu.w Rm,Rn"),
    ("0110nnnnmmmm1011", "neg", "neg Rm,Rn"),
    ("0110nnnnmmmm1010", "negc", "negc Rm,Rn"),
    ("0011nnnnmmmm1000", "sub", "sub Rm,Rn"),
    ("0011nnnnmmmm1010", "subc", "subc Rm,Rn"),
    ("0011nnnnmmmm1011", "subv", "subv Rm,Rn"),
    ("0010nnnnmmmm1001", "and", "and Rm,Rn"),
    ("0110nnnnmmmm0111", "not", "not Rm,Rn"),
    ("0010nnnnmmmm1011", "or", "or Rm,Rn"),
    ("0010nnnnmmmm1000", "tst", "tst Rm,Rn"),
    ("0010nnnnmmmm1010", "xor", "xor Rm,Rn"),
]
for p, op, f in _nm:
    _t(p, op, f, "n m")
# displacement forms
_t("10000000nnnndddd", "mov.b", "mov.b r0,@(disp,Rn)", "d4")     # n in bits 4-7
_t("10000001nnnndddd", "mov.w", "mov.w r0,@(disp,Rn)", "d4w")
_t("0001nnnnmmmmdddd", "mov.l", "mov.l Rm,@(disp,Rn)", "n m d4l")
_t("10000100mmmmdddd", "mov.b", "mov.b @(disp,Rm),r0", "m d4")
_t("10000101mmmmdddd", "mov.w", "mov.w @(disp,Rm),r0", "m d4w")
_t("0101nnnnmmmmdddd", "mov.l", "mov.l @(disp,Rm),Rn", "n m d4l")
_t("11000000dddddddd", "mov.b", "mov.b r0,@(disp,gbr)", "d8")
_t("11000001dddddddd", "mov.w", "mov.w r0,@(disp,gbr)", "d8w")
_t("11000010dddddddd", "mov.l", "mov.l r0,@(disp,gbr)", "d8l")
_t("11000100dddddddd", "mov.b", "mov.b @(disp,gbr),r0", "d8")
_t("11000101dddddddd", "mov.w", "mov.w @(disp,gbr),r0", "d8w")
_t("11000110dddddddd", "mov.l", "mov.l @(disp,gbr),r0", "d8l")
_t("11000111dddddddd", "mova", "mova @T,r0", "pcl")
_t("1001nnnndddddddd", "mov.w", "mov.w @T,Rn", "n pcw")
_t("1101nnnndddddddd", "mov.l", "mov.l @T,Rn", "n pcl")
# immediates
_t("1110nnnniiiiiiii", "mov", "mov #imm,Rn", "n s8")
_t("0111nnnniiiiiiii", "add", "add #imm,Rn", "n s8")
_t("10001000iiiiiiii", "cmp/eq", "cmp/eq #imm,r0", "s8")
_t("11001001iiiiiiii", "and", "and #imm,r0", "u8")
_t("11001011iiiiiiii", "or", "or #imm,r0", "u8")
_t("11001000iiiiiiii", "tst", "tst #imm,r0", "u8")
_t("11001010iiiiiiii", "xor", "xor #imm,r0", "u8")
_t("11001101iiiiiiii", "and.b", "and.b #imm,@(r0,gbr)", "u8")
_t("11001111iiiiiiii", "or.b", "or.b #imm,@(r0,gbr)", "u8")
_t("11001100iiiiiiii", "tst.b", "tst.b #imm,@(r0,gbr)", "u8")
_t("11001110iiiiiiii", "xor.b", "xor.b #imm,@(r0,gbr)", "u8")
_t("11000011iiiiiiii", "trapa", "trapa #imm", "u8")
# branches
_t("10001001dddddddd", "bt", "bt @T", "b8")
_t("10001011dddddddd", "bf", "bf @T", "b8")
_t("10001101dddddddd", "bt/s", "bt/s @T", "b8 delay")
_t("10001111dddddddd", "bf/s", "bf/s @T", "b8 delay")
_t("1010dddddddddddd", "bra", "bra @T", "b12 delay")
_t("1011dddddddddddd", "bsr", "bsr @T", "b12 delay")

# first match wins: order the table from most to least specific
_TABLE.sort(key=lambda e: -bin(e[0]).count("1"))
_CACHE = {}


def _lookup(word):
    e = _CACHE.get(word)
    if e is None:
        e = False
        for mask, val, op, fmt, kind, nsh, msh in _TABLE:
            if word & mask == val:
                e = (op, fmt, kind, nsh, msh)
                break
        _CACHE[word] = e
    return e


def decode(word, pc):
    e = _lookup(word)
    if not e:
        return Insn(pc, word, ".word", ".word 0x%04X" % word)
    op, fmt, kind, nsh, msh = e
    k = kind.split()
    ins = Insn(pc, word, op, fmt, delay="delay" in k)
    if nsh is not None:
        ins.n = (word >> nsh) & 15
    if msh is not None:
        ins.m = (word >> msh) & 15
    d4, d8 = word & 15, word & 0xFF
    if "d4" in k:
        ins.disp = d4
    if "d4w" in k:
        ins.disp = d4 * 2
    if "d4l" in k:
        ins.disp = d4 * 4
    if "d8" in k:
        ins.disp = d8
    if "d8w" in k:
        ins.disp = d8 * 2
    if "d8l" in k:
        ins.disp = d8 * 4
    if "s8" in k:
        ins.imm = _s8(d8)
    if "u8" in k:
        ins.imm = d8
    if "pcw" in k:
        ins.target, ins.size = pc + 4 + d8 * 2, 2
    if "pcl" in k:
        ins.target, ins.size = (pc & ~3) + 4 + d8 * 4, 4
    if "b8" in k:
        ins.target = pc + 4 + _s8(d8) * 2
    if "b12" in k:
        ins.target = pc + 4 + _s12(word & 0xFFF) * 2
    return ins


class Image:
    """A flat binary loaded at `base` (a 1st read file, an overlay...)."""

    def __init__(self, data, base):
        self.data, self.base = data, base
        self.end = base + len(data)

    def contains(self, addr, size=2):
        return self.base <= addr and addr + size <= self.end

    def u16(self, addr):
        return struct.unpack_from(">H", self.data, addr - self.base)[0]

    def u32(self, addr):
        return struct.unpack_from(">I", self.data, addr - self.base)[0]

    def insn(self, addr):
        return decode(self.u16(addr), addr)

    def literal(self, ins):
        """Value a pc-relative mov loads, or None if outside the image."""
        if ins.op in ("mov.w", "mov.l") and ins.size and self.contains(ins.target, ins.size):
            v = self.u16(ins.target) if ins.size == 2 else self.u32(ins.target)
            if ins.size == 2 and v & 0x8000:
                v -= 0x10000
            return v & 0xFFFFFFFF
        return None

    def disasm(self, addr, count):
        out = []
        for i in range(count):
            a = addr + i * 2
            if not self.contains(a):
                break
            ins = self.insn(a)
            s = repr(ins)
            v = self.literal(ins)
            if v is not None:
                s += "    ; =0x%08X" % v
                if v >= 0x00100000 and hw.name(v):
                    s += " " + hw.name(v)
            out.append(s)
        return out

    def words(self):
        for a in range(self.base, self.end - 1, 2):
            yield a, self.u16(a)

    def census(self):
        c = collections.Counter()
        for a, w in self.words():
            c[decode(w, a).op] += 1
        return c

    def literal_refs(self, lo, hi):
        """Every pc-relative literal load whose value lies in [lo, hi)."""
        out = []
        for a, w in self.words():
            if (w >> 12) in (0x9, 0xD):
                ins = decode(w, a)
                v = self.literal(ins)
                if v is not None and lo <= v < hi:
                    out.append((a, v, ins))
        return out


_PROLOGUES = (0x2FE6, 0x4F22)          # mov.l r14,@-r15 ; sts.l pr,@-r15 (SHC/GCC)


def find_base(data, candidates):
    """Rank load addresses for a flat binary.

    For each candidate base, count the pc-relative 32-bit literals that point
    inside the image at a function prologue: the constants a compiler emits
    for `jsr @rn` calls. The right base makes many of them land; a wrong one
    makes almost none. Returns [(score, base)] best first.
    """
    lits = []
    for off in range(0, len(data) - 1, 2):
        w = struct.unpack_from(">H", data, off)[0]
        if w >> 12 == 0xD:
            lit = (off & ~3) + 4 + (w & 0xFF) * 4
            if lit + 4 <= len(data):
                lits.append(struct.unpack_from(">I", data, lit)[0])
    out = []
    for base in candidates:
        score = 0
        for v in lits:
            o = v - base
            if 0 <= o < len(data) - 1 and not o & 1 and \
                    struct.unpack_from(">H", data, o)[0] in _PROLOGUES:
                score += 1
        out.append((score, base))
    return sorted(out, reverse=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file")
    ap.add_argument("--base", default="06004000", help="load address (hex)")
    ap.add_argument("--at", help="disassemble from this address (hex)")
    ap.add_argument("--count", type=int, default=64)
    ap.add_argument("--census", action="store_true")
    ap.add_argument("--refs", help="LO[:HI] hex: literal loads of addresses in the range")
    ap.add_argument("--find-base", action="store_true",
                    help="rank 4 KiB-aligned load addresses in both work RAMs")
    a = ap.parse_args(argv)
    if a.find_base:
        data = open(a.file, "rb").read()
        cands = list(range(0x06000000, 0x06100000, 0x1000)) + list(range(0x00200000, 0x00300000, 0x1000))
        for score, base in find_base(data, cands)[:5]:
            print("0x%08X  %d prologue hits" % (base, score))
        return
    img = Image(open(a.file, "rb").read(), int(a.base, 16))
    if a.at:
        print("\n".join(img.disasm(int(a.at, 16), a.count)))
    if a.census:
        for op, n in img.census().most_common():
            print("%-10s %d" % (op, n))
    if a.refs:
        lo, _, hi = a.refs.partition(":")
        lo = int(lo, 16)
        hi = int(hi, 16) if hi else lo + 1
        for addr, v, ins in img.literal_refs(lo, hi):
            print("%08X  %-28s ; =0x%08X %s" % (addr, ins.text, v, hw.name(v) or ""))


if __name__ == "__main__":
    main()
