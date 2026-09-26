"""The SCU DSP's instruction set: decoding and disassembly.

    python -m saturnkit.scudsp FILE.BIN --base 06004000 --at 0605AB94 --count 256

The SCU's DSP runs a program of up to 256 32-bit words from its own
program RAM, which the SH-2 fills through PPAF/PPD (0x25FE0080/84). Each
word is one of:

* an operation (bits 31-30 = 00): an ALU operation, an X-bus move, a
  Y-bus move and a D1-bus move, all in parallel;
* `MVI` (10): a signed immediate into a register or data RAM, optionally
  on a condition;
* `DMA` (1100): between the A/B bus (D0) and a data RAM or program RAM;
* `JMP` (1101), `BTM`/`LPS` (1110, loops), `END`/`ENDI` (1111).

Field layouts and mnemonics follow Sega's SCU manual (ST-097). The
registers: data RAMs M0-M3 addressed by CT0-CT3 (MCn post-increments CTn),
RX and RY (the multiplier's inputs), P (its product, PH:PL), A (the
accumulator, ACH:ACL), ALU (the ALU's output, ALH:ALL), RA0/WA0 (the DMA
read and write addresses), LOP (the loop count), TOP (the loop start),
PC.
"""
import argparse
import struct

ALU = {0x0: None, 0x1: "AND", 0x2: "OR", 0x3: "XOR", 0x4: "ADD", 0x5: "SUB", 0x6: "AD2",
       0x8: "SR", 0x9: "RR", 0xA: "SL", 0xB: "RL", 0xF: "RL8"}
SRC = ["M0", "M1", "M2", "M3", "MC0", "MC1", "MC2", "MC3"]
D1_SRC = {0: "M0", 1: "M1", 2: "M2", 3: "M3", 4: "MC0", 5: "MC1", 6: "MC2", 7: "MC3", 9: "ALL", 10: "ALH"}
DST = {0: "MC0", 1: "MC1", 2: "MC2", 3: "MC3", 4: "RX", 5: "PL", 6: "RA0", 7: "WA0",
       10: "LOP", 11: "TOP", 12: "CT0", 13: "CT1", 14: "CT2", 15: "CT3"}
MVI_DST = {0: "MC0", 1: "MC1", 2: "MC2", 3: "MC3", 4: "RX", 5: "PL", 6: "RA0", 7: "WA0",
           10: "LOP", 12: "PC"}
DMA_RAM = ["M0", "M1", "M2", "M3", "PRG"]
FLAGS = {1: "Z", 2: "S", 3: "ZS", 4: "C", 8: "T0"}


def _signed(v, bits):
    return v - (1 << bits) if v >> (bits - 1) & 1 else v


def cond(c):
    """A 6-bit condition: bit 5 set tests the flag set, clear tests it clear."""
    name = FLAGS.get(c & 0xF, "?%X" % (c & 0xF))
    return name if c & 0x20 else "N" + name


def disasm(op):
    """One instruction word as text (the manual's mnemonics; parallel moves
    joined by two spaces)."""
    top = op >> 30
    if top == 0:
        parts = []
        alu = ALU.get(op >> 26 & 0xF, "ALU?%X" % (op >> 26 & 0xF))
        if alu:
            parts.append(alu)
        xs, ys = SRC[op >> 20 & 7], SRC[op >> 14 & 7]
        if op >> 25 & 1:
            parts.append("MOV %s,X" % xs)
        xp = op >> 23 & 3
        if xp == 2:
            parts.append("MOV MUL,P")
        elif xp == 3:
            parts.append("MOV %s,P" % xs)
        if op >> 19 & 1:
            parts.append("MOV %s,Y" % ys)
        ya = op >> 17 & 3
        if ya == 1:
            parts.append("CLR A")
        elif ya == 2:
            parts.append("MOV ALU,A")
        elif ya == 3:
            parts.append("MOV %s,A" % ys)
        d1 = op >> 12 & 3
        d = DST.get(op >> 8 & 0xF, "?%X" % (op >> 8 & 0xF))
        if d1 == 1:
            # the immediate is sign-extended; the 8-bit registers keep its low byte
            v = op & 0xFF if op >> 8 & 0xF in (10, 11, 12, 13, 14, 15) else _signed(op & 0xFF, 8)
            parts.append("MOV #%d,%s" % (v, d))
        elif d1 == 3:
            parts.append("MOV %s,%s" % (D1_SRC.get(op & 0xF, "?%X" % (op & 0xF)), d))
        elif d1 == 2:
            parts.append("D1?")
        return "  ".join(parts) or "NOP"
    if top == 2:
        d = MVI_DST.get(op >> 26 & 0xF, "?%X" % (op >> 26 & 0xF))
        if op >> 25 & 1:
            return "MVI #%d,%s,%s" % (_signed(op & 0x7FFFF, 19), d, cond(op >> 19 & 0x3F))
        return "MVI #%d,%s" % (_signed(op & 0x1FFFFFF, 25), d)
    kind = op >> 28 & 3
    if kind == 0:                                   # DMA
        hold = "H" if op >> 14 & 1 else ""
        add = op >> 15 & 7
        step = "" if add == 1 else "0" if add == 0 else str(1 << (add - 1))
        ram = op >> 8 & 7
        ram = DMA_RAM[ram] if ram < 5 else "?%d" % ram
        count = "#%d" % (op & 0xFF) if not op >> 13 & 1 else SRC[op & 7]
        if op >> 12 & 1:
            return "DMA%s%s %s,D0,%s" % (hold, step, ram, count)
        return "DMA%s%s D0,%s,%s" % (hold, step, ram, count)
    if kind == 1:                                   # JMP
        c = op >> 19 & 0x7F
        if c:
            return "JMP %s,$%02X" % (cond(c), op & 0xFF)
        return "JMP $%02X" % (op & 0xFF)
    if kind == 2:
        return "LPS" if op >> 27 & 1 else "BTM"
    return "ENDI" if op >> 27 & 1 else "END"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("file")
    ap.add_argument("--base", default="0", help="load address of the file (hex)")
    ap.add_argument("--at", default=None, help="address of the program's first word (hex; default the base)")
    ap.add_argument("--count", type=int, default=256, help="words (the program RAM holds 256)")
    a = ap.parse_args(argv)
    data = open(a.file, "rb").read()
    base = int(a.base, 16)
    at = int(a.at, 16) if a.at else base
    for k in range(a.count):
        off = at - base + 4 * k
        if off + 4 > len(data):
            break
        op = struct.unpack_from(">I", data, off)[0]
        print("%02X  %08X  %s" % (k, op, disasm(op)))


if __name__ == "__main__":
    main()
