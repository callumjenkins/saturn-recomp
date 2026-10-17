"""68000 decoding and a descent from the vectors, for the programs a game loads into sound RAM.

    python -m saturnrecomp.m68k FILE.BIN [--offset 44] [--bytes 63D4] [--at 1000 --count 32]

A program loads at 0, where the 68000 sees sound RAM, so its first 0x400 bytes are the vector table:
the reset PC at 4 and an exception handler in each long after it. Lengths and text come from Musashi's
disassembler, built on first use into a cache directory, with an indexed operand's extension word read
as the 68000 reads it: brief format, whatever bit 8 says.

The descent starts at every vector that points past the table into the image and follows branches,
calls, dbcc loops and jumps to absolute or PC-relative targets. It reads four shapes of jump table,
bounded by an `andi` mask, or a `cmpi` and a `bcc` or `bhi` past it, on the index on the path to the
jump. A table with no bound runs while its entries are a `bra`, `bsr`, `bcc`, `jmp (d16,PC)` or `rts`,
or up to its lowest target above it, and ends before the first entry whose target turns out bad:

    jmp/jsr (d8,PC,Dn)                                  into a run of branches, Dn the entry's offset
    move.w (d8,PC,Dn),Dm; jmp/jsr (d8,PC,Dm)            words, each an offset from the second table
    movea.w (d8,PC,Dn),An; jmp/jsr (An)                 words, each an address (and movea.l, longs)
    lea (d16,PC),As; movea.l (d8,As,Dn),An; jmp/jsr (An) longs, each an address

Any other jump through a register is unresolved. A path that meets a word that does not decode, an odd
address, an address outside the image or an instruction it would overlap ends there, listed as bad.
"""
import argparse
import bisect
import ctypes
import hashlib
import os
import re
import subprocess
import sys

MUSASHI = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                       "runtime", "third_party", "musashi")
VECTORS = 0x400
FULL = "#define EXT_FULL(A)                       BIT_8(A)"
SHIM = r"""
#include "m68k.h"
static const unsigned char* g_mem;
static unsigned g_len;
static unsigned byte(unsigned a) { return a < g_len ? g_mem[a] : 0; }
unsigned int m68k_read_disassembler_16(unsigned int a) { return byte(a) << 8 | byte(a + 1); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_disassembler_16(a) << 16 | m68k_read_disassembler_16(a + 2); }
unsigned int dasm(const unsigned char* mem, unsigned len, unsigned pc, char* out) {
    g_mem = mem, g_len = len;
    return m68k_disassemble(out, pc, M68K_CPU_TYPE_68000);
}
"""
CONTEXT = 12                                         # instructions before a table jump searched for its bound
RETURNS = {0x4E75, 0x4E73, 0x4E77, 0x4AFC}          # rts, rte, rtr, illegal
REFS = re.compile(r"; \(\$([0-9a-f]+)\)|(?<![#\w])\$([0-9a-f]+)\.[wl]\b")


def library(cache):
    """Musashi's disassembler, built into `cache` once for each version of its source."""
    source = open(os.path.join(MUSASHI, "m68kdasm.c"), encoding="latin-1").read()
    if FULL not in source:
        raise SystemExit(f"{MUSASHI}/m68kdasm.c no longer defines EXT_FULL as {FULL!r}")
    source = source.replace(FULL, "#define EXT_FULL(A) 0")
    key = hashlib.sha1((source + SHIM).encode("latin-1")).hexdigest()[:12]
    path = os.path.join(cache, f"m68kdasm-{key}.so")
    if not os.path.exists(path):
        os.makedirs(cache, exist_ok=True)
        c, shim = os.path.join(cache, f"m68kdasm-{key}.c"), os.path.join(cache, f"shim-{key}.c")
        open(c, "w", encoding="latin-1").write(source)
        open(shim, "w").write(SHIM)
        r = subprocess.run([os.environ.get("CC", "cc"), "-O2", "-shared", "-fPIC", "-I", MUSASHI, "-o", path + ".tmp",
                            shim, c], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"building Musashi's disassembler failed:\n{r.stderr}")
        os.replace(path + ".tmp", path)
    return ctypes.CDLL(path)


class Image:
    def __init__(self, data, lib):
        self.data = bytes(data)
        self._mem = ctypes.create_string_buffer(self.data, len(self.data))
        self._out = ctypes.create_string_buffer(256)
        self._lib = lib
        self._seen = {}

    def __len__(self):
        return len(self.data)

    def u16(self, a):
        return int.from_bytes(self.data[a:a + 2], "big")

    def s16(self, a):
        return int.from_bytes(self.data[a:a + 2], "big", signed=True)

    def u32(self, a):
        return int.from_bytes(self.data[a:a + 4], "big")

    def insn(self, a):
        """(length, text) of the instruction at `a`; text starting "dc.w" is a word that does not decode."""
        if a not in self._seen:
            n = self._lib.dasm(self._mem, len(self.data), a, self._out)
            self._seen[a] = (n, self._out.value.decode("latin-1"))
        return self._seen[a]


def _s8(v):
    return v - 256 if v & 0x80 else v


class Program:
    def __init__(self, image, entries=None):
        self.img = image
        self.entries = sorted(self.vectors() if entries is None else entries)
        self._limits = {}
        while True:
            self.code = {}                # address: length
            self.data = set(range(min(VECTORS, len(image))))
            self.tables = []              # (jump, kind, first byte, entries)
            self.unresolved = set()
            self.bad = {}                 # address: why
            self._owner, self._targets, self._bounded, self._clashed = {}, {}, {}, set()
            self._descend()
            if not self._shorten():
                break

    def _shorten(self):
        """Ends each unbounded table before its first entry whose target came out bad or clashed with
        another path's instruction, as one read past its end does; whether any changed."""
        changed = False
        for jump, targets in self._targets.items():
            if self._bounded[jump]:
                continue
            k = next((k for k, t in enumerate(targets)
                      if t in self.bad or t in self._clashed), None)
            if k is not None:
                self._limits[jump], changed = k, True
        return changed

    def vectors(self):
        img = self.img
        found = {img.u32(a) for a in range(4, min(VECTORS, len(img) - 3), 4)}
        return {v for v in found if VECTORS <= v < len(img) and not v & 1}

    def _descend(self):
        todo = [(e, []) for e in self.entries]
        while todo:
            a, run = todo.pop()
            run = list(run)
            while a not in self.code:
                if a & 1 or not 0 <= a < len(self.img):
                    self.bad[a] = "odd" if a & 1 else "outside the image"
                    break
                n, text = self.img.insn(a)
                clash = next((self._owner[b] for b in range(a, a + n) if b in self._owner), None)
                if clash is not None:
                    self._clashed.add(clash)
                if text.startswith("dc.w") or a + n > len(self.img) or clash is not None:
                    self.bad[a] = ("does not decode" if text.startswith("dc.w") else "past the end"
                                   if clash is None else f"overlaps {clash:X}")
                    break
                self.code[a] = n
                self._owner.update((b, a) for b in range(a, a + n))
                run.append(a)
                targets, falls = self._flow(a, run)
                todo += [(t, run[-CONTEXT:]) for t in targets]
                if not falls:
                    break
                a += n

    def _flow(self, a, run):
        """(targets, whether it falls through) for the instruction at `a`, the last of `run`."""
        img, op = self.img, self.img.u16(a)
        if op in RETURNS:
            return [], False
        if op & 0xF000 == 0x6000:
            d = op & 0xFF
            return [a + 2 + (img.s16(a + 2) if d == 0 else _s8(d))], op & 0x0F00 != 0
        if op & 0xF0F8 == 0x50C8:
            return [a + 2 + img.s16(a + 2)], True
        if op & 0xFF80 == 0x4E80:
            call, mode = not op & 0x40, op & 0x3F
            if mode == 0x38:
                targets = [img.s16(a + 2) & 0xFFFFFF]
            elif mode == 0x39:
                targets = [img.u32(a + 2) & 0xFFFFFF]
            elif mode == 0x3A:
                targets = [a + 2 + img.s16(a + 2)]
            else:
                targets = self._table(a, run)
                if targets is None:
                    self.unresolved.add(a)
                    targets = []
            return targets, call
        return [], True

    def _indexed(self, at, base):
        """(base + the 8-bit displacement, the index register: 0-7 D, 8-15 A) of the operand at `at`."""
        ext = self.img.u16(at + 2)
        return base + _s8(ext & 0xFF), ext >> 12

    def _table(self, a, run):
        img, op = self.img, self.img.u16(a)
        mode, before = op & 0x3F, run[:-1]
        prev = img.u16(before[-1]) if before else None
        if mode == 0x3B:
            table, index = self._indexed(a, a + 2)
            if prev is not None and prev & 0xF1FF == 0x303B and prev >> 9 & 7 == index:
                words, index = self._indexed(before[-1], before[-1] + 2)
                offsets, _ = self._bound(before[:-1], index, before[-1])
                return self._read(a, "offsets", words, 2, offsets, lambda e: table + img.s16(e))
            offsets, scale = self._bound(before, index, a)
            return self._branches(a, table, scale, offsets)
        if mode & 0x38 != 0x10 or prev is None:
            return None
        r = mode & 7
        if prev in (0x307B | r << 9, 0x207B | r << 9):
            table, index = self._indexed(before[-1], before[-1] + 2)
            offsets, _ = self._bound(before[:-1], index, before[-1])
            if prev & 0x1000:
                return self._read(a, "words", table, 2, offsets, lambda e: img.s16(e) & 0xFFFFFF)
            return self._read(a, "longs", table, 4, offsets, lambda e: img.u32(e) & 0xFFFFFF)
        if prev & 0xFFF8 == 0x2070 | r << 9 and len(before) > 1 and img.u16(before[-2]) == 0x41FA | (prev & 7) << 9:
            lea = before[-2]
            table, index = self._indexed(before[-1], lea + 2 + img.s16(lea + 2))
            offsets, _ = self._bound(before[:-2], index, lea)
            return self._read(a, "longs", table, 4, offsets, lambda e: img.u32(e) & 0xFFFFFF)
        return None

    def _bound(self, run, reg, end):
        """(the byte offsets index register `reg` can hold at the end of `run`, a path that goes on to
        `end`, or None, and the factor its shifts and doublings since the bound scale it by)."""
        img, shifts = self.img, []
        if reg >= 8:
            return None, 1
        values = None
        for i in range(len(run) - 1, max(len(run) - CONTEXT, 0) - 1, -1):
            at = run[i]
            op = img.u16(at)
            if op == 0xD040 | reg << 9 | reg:                           # add.w Dn,Dn
                shifts.append(1)
            elif op & 0xF020 == 0xE000 and op & 0xC0 != 0xC0 and op & 0x18 in (0, 8) and op & 7 == reg:
                k = op >> 9 & 7 or 8                                    # asl, asr, lsl or lsr #k,Dn
                shifts.append(k if op & 0x100 else -k)
            elif op in (0x0240 | reg, 0x0200 | reg):                    # andi.w or andi.b #mask,Dn
                mask = img.u16(at + 2) & (0xFFFF if op & 0x40 else 0xFF)
                values = [v for v in range(mask + 1) if v & mask == v] if mask <= 0xFFF else None
                break
            elif op == 0x0C40 | reg and i + 1 < len(run):               # cmpi.w #n,Dn, then bcc or bhi away
                branch = run[i + 1]
                fell = (run[i + 2] if i + 2 < len(run) else end) == branch + img.insn(branch)[0]
                if fell and img.u16(branch) >> 8 in (0x64, 0x62):
                    values = list(range(img.u16(at + 2) + (img.u16(branch) >> 8 == 0x62)))
                break
            elif op & 0xEFF8 == 0x2000 | reg << 9:                       # move.w or move.l Dm,Dn: follow Dm
                reg = op & 7
            elif self._writes(at, reg):
                break
        scale = 1
        for k in reversed(shifts):
            values = values and [v << k if k > 0 else v >> -k for v in values]
            scale = scale << k if k > 0 else max(scale >> -k, 1)
        return (sorted(set(values)) if values else None), scale

    def _writes(self, at, reg):
        """Whether the instruction at `at` may change data register `reg`, by its last operand."""
        text = self.img.insn(at)[1]
        mnemonic, _, operands = text.partition(" ")
        if mnemonic.split(".")[0] in ("cmp", "cmpi", "cmpa", "cmpm", "tst", "btst", "chk"):
            return False
        last = operands.strip().rsplit(", ", 1)[-1]
        for part in last.split("/"):
            m = re.fullmatch(r"D(\d)(?:-D(\d))?", part)
            if m and int(m.group(1)) <= reg <= int(m.group(2) or m.group(1)):
                return True
        return False

    def _branches(self, jump, first, stride, offsets):
        """The entries of a table of branches at `offsets`, or while each starts with a branch or rts."""
        targets = []
        if offsets is not None:
            targets = [first + o for o in offsets]
        else:
            for k in range(self._limits.get(jump, 256)):
                e = first + k * stride
                if not (self.img.u16(e) & 0xF000 == 0x6000 or self.img.u16(e) in (0x4EFA, 0x4E75)):
                    break
                targets.append(e)
        return self._found(jump, "branches", first, targets, offsets is not None)

    def _read(self, jump, kind, first, stride, offsets, target):
        """The targets of a table of `stride`-byte entries, whose bytes become data: those at `offsets`, or
        while each names an even address in the image and the table is short of the lowest above it."""
        if offsets is not None:
            entries = [first + o for o in offsets if first + o + stride <= len(self.img)]
            targets = [target(e) for e in entries]
        else:
            entries, targets, lowest = [], [], len(self.img)
            for k in range(self._limits.get(jump, 256)):
                e = first + k * stride
                if e + stride > len(self.img) or e >= lowest:
                    break
                t = target(e)
                if t & 1 or not VECTORS <= t < len(self.img):
                    break
                entries.append(e)
                targets.append(t)
                lowest = min(lowest, t) if t > first else lowest
        for e in entries:
            self.data.update(range(e, e + stride))
        return self._found(jump, kind, first, targets, offsets is not None)

    def _found(self, jump, kind, first, targets, bounded):
        self.tables.append((jump, kind, first, len(targets)))
        self._targets[jump], self._bounded[jump] = targets, bounded
        return targets

    def code_bytes(self):
        return set(self._owner)

    def refs(self):
        """The addresses discovered instructions name as a (d16,PC) or an absolute operand."""
        out = set()
        for a in self.code:
            for m in REFS.finditer(self.img.insn(a)[1]):
                out.add(int(m.group(1) or m.group(2), 16))
        return out

    def gaps(self):
        """(address, length) of each run of bytes neither code nor data."""
        known, out, start = self.code_bytes() | self.data, [], None
        for b in range(len(self.img) + 1):
            if b < len(self.img) and b not in known:
                start = b if start is None else start
            elif start is not None:
                out.append((start, b - start))
                start = None
        return out


FILL = {b"\x4e\x71", b"\xff\xff", b"\0\0"}            # nop, FFFF, zero


def content(chunk):
    if not chunk.strip(b"\0"):
        return "zero"
    if not len(chunk) & 1 and {chunk[i:i + 2] for i in range(0, len(chunk), 2)} <= FILL:
        return "fill"
    printable = sum(32 <= c < 127 or c in (0, 9, 10, 13) for c in chunk)
    return "text" if len(chunk) >= 8 and printable >= 0.9 * len(chunk) else "other"


def evidence(p):
    """(address, length, content, returns, pointers in, decodes) for each of the program's gaps: the rts
    and rte words at even offsets in it, the addresses discovered code names in it, and, for content
    "other", how many instructions a descent from its first even word past fill decodes, if it stays in
    the gap, ends nowhere bad, resolves every jump and reaches a return; else 0."""
    refs = sorted(p.refs())
    out = []
    for a, n in p.gaps():
        chunk = p.img.data[a:a + n]
        kind = content(chunk)
        returns = sum(1 for i in range(a & 1, n - 1, 2) if chunk[i:i + 2] in (b"\x4e\x75", b"\x4e\x73"))
        pointers = bisect.bisect_left(refs, a + n) - bisect.bisect_left(refs, a)
        out.append((a, n, kind, returns, pointers, _decodes(p, a, n) if kind == "other" else 0))
    return out


def _decodes(p, a, n):
    end, start = a + n, a + (a & 1)
    while start < end and p.img.data[start:start + 2] in FILL:
        start += 2
    if start >= end:
        return 0
    trial = Program(p.img, [start])
    mine = trial.code_bytes()
    if trial.bad or trial.unresolved or any(not a <= b < end for b in mine):
        return 0
    return len(trial.code) if any(p.img.u16(c) in RETURNS for c in trial.code) else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file")
    ap.add_argument("--offset", type=lambda s: int(s, 16), default=0, help="where the program starts in FILE (hex)")
    ap.add_argument("--bytes", type=lambda s: int(s, 16), help="its length (hex); default to the file's end")
    ap.add_argument("--at", type=lambda s: int(s, 16), help="disassemble from here (hex) instead of reporting")
    ap.add_argument("--count", type=int, default=32)
    ap.add_argument("--cache", default=os.path.join(os.path.expanduser("~"), ".cache", "saturnrecomp"))
    a = ap.parse_args(argv)
    data = open(a.file, "rb").read()[a.offset:]
    img = Image(data[:a.bytes] if a.bytes else data, library(a.cache))
    if a.at is not None:
        at = a.at
        for _ in range(a.count):
            n, text = img.insn(at)
            print(f"{at:05X}  {img.data[at:at + n].hex():20}  {text}")
            at += n
        return
    p = Program(img)
    print(f"entries {' '.join(f'{e:X}' for e in p.entries)}; {len(p.code):,} instructions in "
          f"{len(p.code_bytes()):,} bytes, {len(p.data):,} data bytes, {sum(n for _, n in p.gaps()):,} unclassified")
    for jump, kind, first, n in p.tables:
        print(f"  table at {jump:X}: {n} {kind} from {first:X}")
    for at in sorted(p.unresolved):
        print(f"  unresolved {at:X}  {img.insn(at)[1]}")
    for at, why in sorted(p.bad.items()):
        print(f"  bad {at:X}: {why}")


if __name__ == "__main__":
    sys.exit(main())
