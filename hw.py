"""The Saturn's address map and register names, for annotating code.

    python -m saturnkit.hw 25D00002 25F80114 06000310

name(addr) returns "VDP1.FBCR", "WRAM-H", "BIOS.SYS_SETSINT"... or None.

The SH-2 sees a 27-bit external space mirrored through its cache areas:
0x0xxxxxxx cached, 0x2xxxxxxx cache-through (the same memory), 0x4xxxxxxx
associative purge (a write purges the matching cache line), 0x6xxxxxxx
address array, 0xCxxxxxxx the cache's data array used as 4 KiB of RAM,
0xFFFFFE00-0xFFFFFFFF the on-chip peripherals. canonical() folds the
first two onto one address.

Register offsets are from the hardware manuals (VDP1 ST-013, VDP2 ST-058,
SCU ST-097, SMPC ST-169, SH7604 hardware manual). The BIOS entries are the
service pointers the SBL headers call through; `checked` marks those seen
used by a game, with the arguments that confirm the role.
"""
import sys

REGIONS = [
    (0x00000000, 0x00080000, "BIOS ROM"),
    (0x00100000, 0x00100080, "SMPC"),
    (0x00180000, 0x00190000, "backup RAM"),
    (0x00200000, 0x00300000, "WRAM-L"),
    (0x01000000, 0x01000004, "SINIT (wake slave)"),
    (0x01800000, 0x01800004, "MINIT (wake master)"),
    (0x02000000, 0x05000000, "A-bus cartridge"),
    (0x05800000, 0x05900000, "CD block"),
    (0x05A00000, 0x05A80000, "SCSP RAM"),
    (0x05B00000, 0x05B00EE4, "SCSP"),
    (0x05C00000, 0x05C80000, "VDP1 VRAM"),
    (0x05C80000, 0x05D00000, "VDP1 framebuffer"),
    (0x05D00000, 0x05D00018, "VDP1"),
    (0x05E00000, 0x05E80000, "VDP2 VRAM"),
    (0x05F00000, 0x05F01000, "VDP2 CRAM"),
    (0x05F80000, 0x05F80120, "VDP2"),
    (0x05FE0000, 0x05FE00D0, "SCU"),
    (0x06000000, 0x06100000, "WRAM-H"),
]

VDP1 = {0x00: "TVMR", 0x02: "FBCR", 0x04: "PTMR", 0x06: "EWDR", 0x08: "EWLR",
        0x0A: "EWRR", 0x0C: "ENDR", 0x10: "EDSR", 0x12: "LOPR", 0x14: "COPR",
        0x16: "MODR"}

VDP2 = dict(zip(range(0, 0x120, 2), """
TVMD EXTEN TVSTAT VRSIZE HCNT VCNT - RAMCTL CYCA0L CYCA0U CYCA1L CYCA1U CYCB0L
CYCB0U CYCB1L CYCB1U BGON MZCTL SFSEL SFCODE CHCTLA CHCTLB BMPNA BMPNB PNCN0
PNCN1 PNCN2 PNCN3 PNCR PLSZ MPOFN MPOFR MPABN0 MPCDN0 MPABN1 MPCDN1 MPABN2
MPCDN2 MPABN3 MPCDN3 MPABRA MPCDRA MPEFRA MPGHRA MPIJRA MPKLRA MPMNRA MPOPRA
MPABRB MPCDRB MPEFRB MPGHRB MPIJRB MPKLRB MPMNRB MPOPRB SCXIN0 SCXDN0 SCYIN0
SCYDN0 ZMXIN0 ZMXDN0 ZMYIN0 ZMYDN0 SCXIN1 SCXDN1 SCYIN1 SCYDN1 ZMXIN1 ZMXDN1
ZMYIN1 ZMYDN1 SCXN2 SCYN2 SCXN3 SCYN3 ZMCTL SCRCTL VCSTAU VCSTAL LSTA0U LSTA0L
LSTA1U LSTA1L LCTAU LCTAL BKTAU BKTAL RPMD RPRCTL KTCTL KTAOF OVPNRA OVPNRB
RPTAU RPTAL WPSX0 WPSY0 WPEX0 WPEY0 WPSX1 WPSY1 WPEX1 WPEY1 WCTLA WCTLB WCTLC
WCTLD LWTA0U LWTA0L LWTA1U LWTA1L SPCTL SDCTL CRAOFA CRAOFB LNCLEN SFPRMD CCCTL
SFCCMD PRISA PRISB PRISC PRISD PRINA PRINB PRIR - CCRSA CCRSB CCRSC CCRSD CCRNA
CCRNB CCRR CCRLB CLOFEN CLOFSL COAR COAG COAB COBR COBG COBB""".split()))

SCU = {0x90: "T0C", 0x94: "T1S", 0x98: "T1MD", 0xA0: "IMS", 0xA4: "IST",
       0xA8: "AIACK", 0xB0: "ASR0", 0xB4: "ASR1", 0xB8: "AREF", 0xC4: "RSEL",
       0xC8: "VER", 0x60: "DSTP", 0x7C: "DSTA", 0x80: "PPAF", 0x84: "PPD",
       0x88: "PDA", 0x8C: "PDD"}
for _lvl in range(3):
    for _o, _n in ((0x00, "R"), (0x04, "W"), (0x08, "C"), (0x0C, "AD"), (0x10, "EN"), (0x14, "MD")):
        SCU[_lvl * 0x20 + _o] = "D%d%s" % (_lvl, _n)

SMPC = {0x1F: "COMREG", 0x61: "SR", 0x63: "SF", 0x75: "PDR1", 0x77: "PDR2",
        0x79: "DDR1", 0x7B: "DDR2", 0x7D: "IOSEL", 0x7F: "EXLE"}
for _i in range(7):
    SMPC[0x01 + 2 * _i] = "IREG%d" % _i
for _i in range(32):
    SMPC[0x21 + 2 * _i] = "OREG%d" % _i

CDBLOCK = {0x90008: "HIRQ", 0x9000C: "HIRQMASK", 0x90018: "CR1", 0x9001C: "CR2",
           0x90020: "CR3", 0x90024: "CR4", 0x18000: "DATATRNS", 0x98000: "DATATRNS"}

ONCHIP = {0xFE00: "SMR", 0xFE01: "BRR", 0xFE02: "SCR", 0xFE03: "TDR", 0xFE04: "SSR",
          0xFE05: "RDR", 0xFE10: "TIER", 0xFE11: "FTCSR", 0xFE12: "FRCH", 0xFE13: "FRCL",
          0xFE14: "OCRH", 0xFE15: "OCRL", 0xFE16: "TCR", 0xFE17: "TOCR", 0xFE18: "FICRH",
          0xFE19: "FICRL", 0xFE60: "IPRB", 0xFE62: "VCRA", 0xFE64: "VCRB", 0xFE66: "VCRC",
          0xFE68: "VCRD", 0xFE71: "DRCR0", 0xFE72: "DRCR1", 0xFE80: "WTCSR", 0xFE81: "WTCNT",
          0xFE83: "RSTCSR", 0xFE91: "SBYCR", 0xFE92: "CCR", 0xFEE0: "ICR", 0xFEE2: "IPRA",
          0xFEE4: "VCRWDT", 0xFF00: "DVSR", 0xFF04: "DVDNT", 0xFF08: "DVCR", 0xFF0C: "VCRDIV",
          0xFF10: "DVDNTH", 0xFF14: "DVDNTL", 0xFF80: "SAR0", 0xFF84: "DAR0", 0xFF88: "TCR0",
          0xFF8C: "CHCR0", 0xFF90: "SAR1", 0xFF94: "DAR1", 0xFF98: "TCR1", 0xFF9C: "CHCR1",
          0xFFA0: "VCRDMA0", 0xFFA8: "VCRDMA1", 0xFFB0: "DMAOR", 0xFFE0: "BCR1",
          0xFFE4: "BCR2", 0xFFE8: "WCR", 0xFFEC: "MCR", 0xFFF0: "RTCSR", 0xFFF4: "RTCNT",
          0xFFF8: "RTCOR"}

# BIOS service pointers in WRAM-H, as the SBL headers name them.
# (address, name, checked-by-a-game note or "")
BIOS = [
    (0x0600026C, "SYS_EXECDMP?", ""),
    (0x06000300, "SYS_SETUINT", "Virtual Hydlide: (0x40 VBlank-IN, handler), (0x4D sprite end, handler)"),
    (0x06000304, "SYS_GETUINT", "Virtual Hydlide: returns the handler it saves before replacing it"),
    (0x06000310, "SYS_SETSINT", "Virtual Hydlide: (0x94, slave entry) before SSHON"),
    (0x06000314, "SYS_GETSINT", ""),
    (0x06000320, "SYS_CHGSYSCK", ""),
    (0x06000324, "SYS_GETSYSCK", "a variable, read"),
    (0x06000330, "SYS_TASSEM", "Virtual Hydlide: spins on it with semaphore numbers 0x00, 0x20-0x23"),
    (0x06000334, "SYS_CLRSEM", "Virtual Hydlide: paired with SYS_TASSEM on the same numbers"),
    (0x06000340, "SYS_SETSCUIM", ""),
    (0x06000344, "SYS_CHGSCUIM", "Virtual Hydlide: (and-mask, or-mask) pairs"),
    (0x06000348, "SYS_GETSCUIM", "a variable, read"),
    (0x06000354, "BUP vector table", "Virtual Hydlide: backup-memory utility calls through it"),
    (0x06000358, "BUP_INIT", "Virtual Hydlide: called once with the library buffer"),
]
BIOS_NAMES = {a: n for a, n, _ in BIOS}

# SCU interrupt vectors (SYS_SETUINT numbers)
SCU_VECTORS = {0x40: "VBlank-IN", 0x41: "VBlank-OUT", 0x42: "HBlank-IN", 0x43: "Timer 0",
               0x44: "Timer 1", 0x45: "DSP end", 0x46: "sound request", 0x47: "SMPC",
               0x48: "PAD", 0x49: "DMA level 2 end", 0x4A: "DMA level 1 end",
               0x4B: "DMA level 0 end", 0x4C: "DMA illegal", 0x4D: "sprite draw end",
               0x50: "A-bus"}


def canonical(addr):
    """Fold the cache-through mirror onto the cached address."""
    addr &= 0xFFFFFFFF
    if addr >> 28 in (0x0, 0x2):
        return addr & 0x07FFFFFF
    return addr


def name(addr):
    addr &= 0xFFFFFFFF
    if addr in BIOS_NAMES:
        return "BIOS." + BIOS_NAMES[addr]
    if addr >= 0xFFFFFE00:
        r = ONCHIP.get(addr & 0xFFFF)
        return "SH2." + r if r else "SH2 on-chip"
    if addr >> 28 not in (0x0, 0x2):
        return None
    c = canonical(addr)
    for lo, hi, region in REGIONS:
        if lo <= c < hi:
            off = c - lo
            table = {"VDP1": VDP1, "VDP2": VDP2, "SCU": SCU, "SMPC": SMPC, "CD block": CDBLOCK}.get(region)
            reg = table.get(off) if table else None
            label = region + ("." + reg if reg else "" if table is None else "+0x%X" % off)
            if addr >> 28 == 0x2 and region in ("WRAM-L", "WRAM-H"):
                label += " (cache-through)"
            return label
    return None


def main(argv=None):
    for a in (argv if argv is not None else sys.argv[1:]):
        print("%08X  %s" % (int(a, 16), name(int(a, 16))))


if __name__ == "__main__":
    main()
