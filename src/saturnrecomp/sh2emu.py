"""An SH-2 interpreter, for running isolated guest functions.

    python -m saturnrecomp.sh2emu FILE.BIN --base 0600B000 --call 0603E918 --regs r0=7,r1=100

It is the reference the recompiler is checked against, and a way to ask
the game's own code a question (what does this helper compute?) without an
emulator. It models the CPU only: registers, SR (T, S, Q, M, the interrupt
mask), GBR, VBR, MACH/MACL, PR, delay slots. Memory is a set of flat
regions, big-endian; an access outside them raises, so a function that
touches hardware says so instead of computing nonsense.

`CPU.call(entry, r4=.., r5=.., ...)` sets the arguments, points PR at a
return sentinel and runs until the function returns.
"""
import argparse
import struct

from . import sh2

RETURN_SENTINEL = 0xFFFFFFF0
M32 = 0xFFFFFFFF


def s32(x):
    x &= M32
    return x - (1 << 32) if x & 0x80000000 else x


def s16(x):
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


def s8(x):
    x &= 0xFF
    return x - 0x100 if x & 0x80 else x


class MemoryError_(Exception):
    pass


class Memory:
    """Flat big-endian regions; the cache-through mirror folded onto the cached address."""

    def __init__(self):
        self.regions = []            # (lo, hi, bytearray)
        self.log = None              # set to a list to record (kind, addr, size, value)

    def add(self, lo, size, data=None):
        buf = bytearray(size)
        if data:
            buf[:len(data)] = data
        self.regions.append((lo, lo + size, buf))
        return buf

    def _find(self, addr, size):
        a = addr & M32
        if a >> 28 == 0x2:                # cache-through: the same memory
            a &= 0x1FFFFFFF
        for lo, hi, buf in self.regions:
            if lo <= a and a + size <= hi:
                return buf, a - lo
        raise MemoryError_("unmapped %d-byte access at %08X" % (size, addr & M32))

    def read(self, addr, size):
        buf, o = self._find(addr, size)
        v = int.from_bytes(buf[o:o + size], "big")
        if self.log is not None:
            self.log.append(("r", addr & M32, size, v))
        return v

    def write(self, addr, size, value):
        buf, o = self._find(addr, size)
        buf[o:o + size] = (value & ((1 << (8 * size)) - 1)).to_bytes(size, "big")
        if self.log is not None:
            self.log.append(("w", addr & M32, size, value & ((1 << (8 * size)) - 1)))


class CPU:
    def __init__(self, mem):
        self.mem = mem
        self.r = [0] * 16
        self.pc = 0
        self.pr = 0
        self.gbr = self.vbr = 0
        self.mach = self.macl = 0
        self.t = self.s = self.q = self.m = 0
        self.imask = 15
        self.steps = 0
        self._decoded = {}

    # -- SR as a word
    @property
    def sr(self):
        return (self.m << 9) | (self.q << 8) | (self.imask << 4) | (self.s << 1) | self.t

    @sr.setter
    def sr(self, v):
        self.m, self.q = (v >> 9) & 1, (v >> 8) & 1
        self.imask, self.s, self.t = (v >> 4) & 15, (v >> 1) & 1, v & 1

    # -- memory helpers
    def rb(self, a): return self.mem.read(a, 1)
    def rw(self, a): return self.mem.read(a, 2)
    def rl(self, a): return self.mem.read(a, 4)
    def wb(self, a, v): self.mem.write(a, 1, v)
    def ww(self, a, v): self.mem.write(a, 2, v)
    def wl(self, a, v): self.mem.write(a, 4, v)

    def fetch(self, pc):
        ins = self._decoded.get(pc)
        if ins is None:
            ins = sh2.decode(self.mem.read(pc, 2), pc)
            self._decoded[pc] = ins
        return ins

    def call(self, entry, max_steps=10_000_000, **regs):
        for k, v in regs.items():
            self.r[int(k[1:])] = v & M32
        self.pr = RETURN_SENTINEL
        self.pc = entry
        while self.pc != RETURN_SENTINEL:
            self.step()
            if self.steps > max_steps:
                raise RuntimeError("no return after %d steps (pc %08X)" % (max_steps, self.pc))
        return self.r[0]

    def step(self):
        pc = self.pc
        ins = self.fetch(pc)
        self.steps += 1
        target = self.exec(ins, pc)
        if target is None:
            self.pc = (pc + 2) & M32
        elif ins.delay:
            slot = self.fetch(pc + 2)
            if slot.delay:
                raise RuntimeError("branch in a delay slot at %08X" % (pc + 2))
            self.steps += 1
            self.exec(slot, pc + 2, in_slot=True)
            self.pc = target & M32
        else:
            self.pc = target & M32

    # -- execution: returns the branch target, or None to fall through
    def exec(self, ins, pc, in_slot=False):
        r = self.r
        op, f, n, m = ins.op, ins.fmt, ins.n, ins.m
        imm = ins.imm

        if op == "nop":
            return None
        # ----- moves
        if op == "mov":
            if imm is not None:
                r[n] = imm & M32
            else:
                r[n] = r[m]
            return None
        if op in ("mov.b", "mov.w", "mov.l"):
            size = {"mov.b": 1, "mov.w": 2, "mov.l": 4}[op]
            return self._mov(ins, f, size, n, m, pc)
        if op == "mova":
            r[0] = ins.target & M32
            return None
        if op == "movt":
            r[n] = self.t
            return None
        if op == "swap.b":
            v = r[m]
            r[n] = (v & 0xFFFF0000) | ((v & 0xFF) << 8) | ((v >> 8) & 0xFF)
            return None
        if op == "swap.w":
            v = r[m]
            r[n] = ((v << 16) | (v >> 16)) & M32
            return None
        if op == "xtrct":
            r[n] = ((r[m] << 16) | (r[n] >> 16)) & M32
            return None
        # ----- arithmetic
        if op == "add":
            r[n] = (r[n] + (imm if imm is not None else r[m])) & M32
            return None
        if op == "addc":
            tmp = r[n] + r[m] + self.t
            self.t = 1 if tmp > M32 else 0
            r[n] = tmp & M32
            return None
        if op == "addv":
            res = s32(r[n]) + s32(r[m])
            self.t = 1 if res > 0x7FFFFFFF or res < -0x80000000 else 0
            r[n] = res & M32
            return None
        if op == "sub":
            r[n] = (r[n] - r[m]) & M32
            return None
        if op == "subc":
            tmp = r[n] - r[m] - self.t
            self.t = 1 if tmp < 0 else 0
            r[n] = tmp & M32
            return None
        if op == "subv":
            res = s32(r[n]) - s32(r[m])
            self.t = 1 if res > 0x7FFFFFFF or res < -0x80000000 else 0
            r[n] = res & M32
            return None
        if op == "neg":
            r[n] = (-r[m]) & M32
            return None
        if op == "negc":
            tmp = -r[m] - self.t
            self.t = 1 if tmp < 0 else 0
            r[n] = tmp & M32
            return None
        if op == "dt":
            r[n] = (r[n] - 1) & M32
            self.t = 1 if r[n] == 0 else 0
            return None
        if op in ("exts.b", "exts.w", "extu.b", "extu.w"):
            v = r[m]
            r[n] = {"exts.b": s8(v) & M32, "exts.w": s16(v) & M32,
                    "extu.b": v & 0xFF, "extu.w": v & 0xFFFF}[op]
            return None
        # ----- compare
        if op.startswith("cmp/"):
            a = r[n] if "Rn" in f else r[0]
            if op == "cmp/eq":
                b = (imm & M32) if imm is not None else r[m]
                self.t = 1 if a == b else 0
            elif op == "cmp/hs":
                self.t = 1 if r[n] >= r[m] else 0
            elif op == "cmp/ge":
                self.t = 1 if s32(r[n]) >= s32(r[m]) else 0
            elif op == "cmp/hi":
                self.t = 1 if r[n] > r[m] else 0
            elif op == "cmp/gt":
                self.t = 1 if s32(r[n]) > s32(r[m]) else 0
            elif op == "cmp/pz":
                self.t = 1 if s32(r[n]) >= 0 else 0
            elif op == "cmp/pl":
                self.t = 1 if s32(r[n]) > 0 else 0
            elif op == "cmp/str":
                x = r[n] ^ r[m]
                self.t = 1 if any(((x >> s) & 0xFF) == 0 for s in (0, 8, 16, 24)) else 0
            return None
        # ----- logic
        if op in ("and", "or", "xor", "tst"):
            if imm is not None:
                a, b, dst = r[0], imm, 0
            else:
                a, b, dst = r[n], r[m], n
            v = a & b if op in ("and", "tst") else a | b if op == "or" else a ^ b
            if op == "tst":
                self.t = 1 if v == 0 else 0
            else:
                r[dst] = v & M32
            return None
        if op in ("and.b", "or.b", "xor.b", "tst.b"):
            ea = (self.gbr + r[0]) & M32
            v = self.rb(ea)
            if op == "tst.b":
                self.t = 1 if v & imm == 0 else 0
            else:
                self.wb(ea, {"and.b": v & imm, "or.b": v | imm, "xor.b": v ^ imm}[op])
            return None
        if op == "not":
            r[n] = (~r[m]) & M32
            return None
        if op == "tas.b":
            v = self.rb(r[n])
            self.t = 1 if v == 0 else 0
            self.wb(r[n], v | 0x80)
            return None
        # ----- shifts and rotates
        if op in ("shll", "shal"):
            self.t = r[n] >> 31
            r[n] = (r[n] << 1) & M32
            return None
        if op == "shlr":
            self.t = r[n] & 1
            r[n] >>= 1
            return None
        if op == "shar":
            self.t = r[n] & 1
            r[n] = (s32(r[n]) >> 1) & M32
            return None
        if op in ("shll2", "shll8", "shll16"):
            r[n] = (r[n] << int(op[4:])) & M32
            return None
        if op in ("shlr2", "shlr8", "shlr16"):
            r[n] >>= int(op[4:])
            return None
        if op == "rotl":
            self.t = r[n] >> 31
            r[n] = ((r[n] << 1) | self.t) & M32
            return None
        if op == "rotr":
            self.t = r[n] & 1
            r[n] = (r[n] >> 1) | (self.t << 31)
            return None
        if op == "rotcl":
            t = r[n] >> 31
            r[n] = ((r[n] << 1) | self.t) & M32
            self.t = t
            return None
        if op == "rotcr":
            t = r[n] & 1
            r[n] = (r[n] >> 1) | (self.t << 31)
            self.t = t
            return None
        # ----- multiply and divide
        if op == "mul.l":
            self.macl = (r[n] * r[m]) & M32
            return None
        if op == "muls.w":
            self.macl = (s16(r[n]) * s16(r[m])) & M32
            return None
        if op == "mulu.w":
            self.macl = ((r[n] & 0xFFFF) * (r[m] & 0xFFFF)) & M32
            return None
        if op in ("dmuls.l", "dmulu.l"):
            p = s32(r[n]) * s32(r[m]) if op == "dmuls.l" else r[n] * r[m]
            p &= (1 << 64) - 1
            self.mach, self.macl = p >> 32, p & M32
            return None
        if op == "mac.w":
            a = s16(self.rw(r[n]))
            r[n] = (r[n] + 2) & M32
            b = s16(self.rw(r[m]))
            r[m] = (r[m] + 2) & M32
            p = a * b
            if self.s:
                acc = s32(self.macl) + p
                if acc > 0x7FFFFFFF:
                    acc, self.mach = 0x7FFFFFFF, self.mach | 1
                elif acc < -0x80000000:
                    acc, self.mach = -0x80000000, self.mach | 1
                self.macl = acc & M32
            else:
                acc = ((self.mach << 32) | self.macl)
                acc = (acc - (1 << 64) if acc >> 63 else acc) + p
                acc &= (1 << 64) - 1
                self.mach, self.macl = acc >> 32, acc & M32
            return None
        if op == "mac.l":
            a = s32(self.rl(r[n]))
            r[n] = (r[n] + 4) & M32
            b = s32(self.rl(r[m]))
            r[m] = (r[m] + 4) & M32
            acc = ((self.mach << 32) | self.macl)
            acc = (acc - (1 << 64) if acc >> 63 else acc) + a * b
            if self.s:                                   # saturate to 48 bits
                acc = max(min(acc, (1 << 47) - 1), -(1 << 47))
            acc &= (1 << 64) - 1
            self.mach, self.macl = acc >> 32, acc & M32
            return None
        if op == "clrmac":
            self.mach = self.macl = 0
            return None
        if op == "div0u":
            self.m = self.q = self.t = 0
            return None
        if op == "div0s":
            self.q = r[n] >> 31
            self.m = r[m] >> 31
            self.t = 1 if self.q != self.m else 0
            return None
        if op == "div1":
            self._div1(n, m)
            return None
        # ----- T and system
        if op == "clrt":
            self.t = 0
            return None
        if op == "sett":
            self.t = 1
            return None
        if op in ("ldc", "ldc.l", "lds", "lds.l"):
            reg = f.split(",")[1]
            if op.endswith(".l"):
                v = self.rl(r[m])
                r[m] = (r[m] + 4) & M32
            else:
                v = r[m]
            self._setreg(reg, v)
            return None
        if op in ("stc", "stc.l", "sts", "sts.l"):
            reg = f.split()[1].split(",")[0]
            v = self._getreg(reg)
            if op.endswith(".l"):
                r[n] = (r[n] - 4) & M32
                self.wl(r[n], v)
            else:
                r[n] = v & M32
            return None
        # ----- branches
        if op in ("bt", "bf"):
            taken = self.t == (op == "bt")
            return ins.target if taken else None
        if op in ("bt/s", "bf/s"):
            taken = self.t == (op == "bt/s")
            return ins.target if taken else pc + 4
        if op == "bra":
            return ins.target
        if op == "bsr":
            self.pr = (pc + 4) & M32
            return ins.target
        if op == "braf":
            return (pc + 4 + r[n]) & M32
        if op == "bsrf":
            self.pr = (pc + 4) & M32
            return (pc + 4 + r[n]) & M32
        if op == "jmp":
            return r[n]
        if op == "jsr":
            t = r[n]
            self.pr = (pc + 4) & M32
            return t
        if op == "rts":
            return self.pr
        if op == "rte":
            ret = self.rl(r[15])
            r[15] = (r[15] + 4) & M32
            self.sr = self.rl(r[15]) & 0x3F3
            r[15] = (r[15] + 4) & M32
            return ret
        if op == "trapa":
            raise RuntimeError("trapa #%d at %08X" % (imm, pc))
        if op == "sleep":
            raise RuntimeError("sleep at %08X" % pc)
        raise RuntimeError("cannot execute %r at %08X" % (ins.text, pc))

    def _getreg(self, name):
        return {"sr": self.sr, "gbr": self.gbr, "vbr": self.vbr,
                "mach": self.mach, "macl": self.macl, "pr": self.pr}[name]

    def _setreg(self, name, v):
        v &= M32
        if name == "sr":
            self.sr = v & 0x3F3
        elif name == "mach":
            self.mach = v
        else:
            setattr(self, name, v)

    def _mov(self, ins, f, size, n, m, pc):
        r = self.r
        rd = {1: self.rb, 2: self.rw, 4: self.rl}[size]
        wr = {1: self.wb, 2: self.ww, 4: self.wl}[size]
        ext = {1: s8, 2: s16, 4: lambda x: x}[size]
        d = ins.disp
        if f.endswith("@T,Rn"):                        # pc-relative literal
            r[n] = ext(rd(ins.target)) & M32
        elif f == "%s Rm,@Rn" % ins.op:
            wr(r[n], r[m])
        elif f == "%s @Rm,Rn" % ins.op:
            r[n] = ext(rd(r[m])) & M32
        elif f == "%s Rm,@-Rn" % ins.op:
            v = r[m]
            r[n] = (r[n] - size) & M32
            wr(r[n], v)
        elif f == "%s @Rm+,Rn" % ins.op:
            a = r[m]
            if n != m:
                r[m] = (r[m] + size) & M32
            r[n] = ext(rd(a)) & M32
        elif f == "%s Rm,@(r0,Rn)" % ins.op:
            wr((r[0] + r[n]) & M32, r[m])
        elif f == "%s @(r0,Rm),Rn" % ins.op:
            r[n] = ext(rd((r[0] + r[m]) & M32)) & M32
        elif f == "%s r0,@(disp,Rn)" % ins.op:
            wr((r[n] + d) & M32, r[0])
        elif f == "%s @(disp,Rm),r0" % ins.op:
            r[0] = ext(rd((r[m] + d) & M32)) & M32
        elif f == "mov.l Rm,@(disp,Rn)":
            wr((r[n] + d) & M32, r[m])
        elif f == "mov.l @(disp,Rm),Rn":
            r[n] = rd((r[m] + d) & M32)
        elif f == "%s r0,@(disp,gbr)" % ins.op:
            wr((self.gbr + d) & M32, r[0])
        elif f == "%s @(disp,gbr),r0" % ins.op:
            r[0] = ext(rd((self.gbr + d) & M32)) & M32
        else:
            raise RuntimeError("unhandled move %r at %08X" % (ins.text, pc))
        return None

    def _div1(self, n, m):
        """One step of non-restoring division, as the SH-2 manual defines it."""
        r = self.r
        old_q = self.q
        self.q = r[n] >> 31
        r[n] = ((r[n] << 1) | self.t) & M32
        if old_q == 0:
            if self.m == 0:
                tmp0 = r[n]
                r[n] = (r[n] - r[m]) & M32
                tmp1 = 1 if r[n] > tmp0 else 0
                self.q = tmp1 if self.q == 0 else (1 if tmp1 == 0 else 0)
            else:
                tmp0 = r[n]
                r[n] = (r[n] + r[m]) & M32
                tmp1 = 1 if r[n] < tmp0 else 0
                self.q = (1 if tmp1 == 0 else 0) if self.q == 0 else tmp1
        else:
            if self.m == 0:
                tmp0 = r[n]
                r[n] = (r[n] + r[m]) & M32
                tmp1 = 1 if r[n] < tmp0 else 0
                self.q = tmp1 if self.q == 0 else (1 if tmp1 == 0 else 0)
            else:
                tmp0 = r[n]
                r[n] = (r[n] - r[m]) & M32
                tmp1 = 1 if r[n] > tmp0 else 0
                self.q = (1 if tmp1 == 0 else 0) if self.q == 0 else tmp1
        self.t = 1 if self.q == self.m else 0


def load(path, base, stack_top=0x06100000):
    """A CPU with a flat program loaded at `base`, WRAM-H and WRAM-L mapped."""
    mem = Memory()
    data = open(path, "rb").read()
    hi = mem.add(0x06000000, 0x100000)
    if 0x06000000 <= base < 0x06100000:
        hi[base - 0x06000000:base - 0x06000000 + len(data)] = data
    mem.add(0x00200000, 0x100000)
    cpu = CPU(mem)
    cpu.r[15] = stack_top - 0x10
    return cpu


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file")
    ap.add_argument("--base", default="0600B000")
    ap.add_argument("--call", required=True, help="entry (hex)")
    ap.add_argument("--regs", default="", help="r0=..,r4=.. (decimal or 0x)")
    ap.add_argument("--trace", action="store_true")
    a = ap.parse_args(argv)
    cpu = load(a.file, int(a.base, 16))
    regs = {}
    for kv in filter(None, a.regs.split(",")):
        k, v = kv.split("=")
        regs[k] = int(v, 0)
    if a.trace:
        cpu.mem.log = []
    r0 = cpu.call(int(a.call, 16), **regs)
    print("r0 = 0x%08X (%d)   %d steps" % (r0, s32(r0), cpu.steps))
    print(" ".join("r%d=%08X" % (i, v) for i, v in enumerate(cpu.r)))
    if a.trace:
        for kind, addr, size, v in cpu.mem.log:
            print("  %s%d %08X = %X" % (kind, size * 8, addr, v))


if __name__ == "__main__":
    main()
