// saturn-recomp runtime — the SCU DSP: its registers and an interpreter.
//
// The SH-2 reaches the DSP through four SCU registers: PPAF (0x80, program
// control: run, load the PC, the flags), PPD (0x84, a program word at the
// PC), PDA (0x88, a data RAM address: bank in bits 7-6, word in 5-0) and
// PDD (0x8C, a data word at that address, which then moves on). A program
// is up to 256 words; the data RAM is four banks of 64 words, M0-M3,
// addressed by CT0-CT3.
//
// The DSP runs when PPAF's EX bit is written and stops at END/ENDI (which
// also raises the SCU's DSP-end interrupt). Here it runs to its end at
// once, in the SH-2's write, and its DMA transfers complete at once, so a
// program never waits on T0 and the SH-2 always sees it finished: fine for
// a program the CPU starts and polls, which is how SGL-era code uses it.
//
// Semantics after Sega's SCU manual (ST-097). In one operation word the ALU
// works on the A and P registers as they were, then the X and Y buses move
// (a load of A from ALU takes this operation's result), the data RAM
// counters step once for every bank read, then the D1 bus moves (it reads
// the ALU's new value). The multiplier holds RX * RY from the previous
// operation. Jumps and loops have a delay slot: the next word runs first.
#include "saturn.h"
#include <cstring>

namespace {
struct Dsp {
    uint32_t prog[256];
    uint32_t ram[4][64];
    uint8_t pc, ra, ct[4], top;
    uint16_t lop;
    bool ex, end, z, s, c, v;
    int32_t rx, ry;
    int64_t mul;                       // RX * RY, 48 bits used
    uint64_t alu;                      // 48 bits
    int64_t p, a;                      // PH:PL and ACH:ACL, 48 bits, sign-extended
    uint32_t ra0, wa0;
};
Dsp g;

int64_t sext48(int64_t v) { return (int64_t)((uint64_t)v << 16) >> 16; }

uint32_t lo32(int64_t v) { return (uint32_t)v; }
}

// ---- data RAM access through the counters --------------------------------------------------
static uint32_t read_src(int s, int bump[4]) {       // 0-3 Mn, 4-7 MCn (the counter steps later)
    int b = s & 3;
    if (s & 4) bump[b] = 1;
    return g.ram[b][g.ct[b] & 63];
}

static uint32_t read_d1(int s) {                     // D1 bus sources: Mn, MCn (steps now), ALL, ALH
    if (s < 8) {
        int b = s & 3;
        uint32_t v = g.ram[b][g.ct[b] & 63];
        if (s & 4) g.ct[b] = (g.ct[b] + 1) & 63;
        return v;
    }
    if (s == 9) return (uint32_t)g.alu;
    if (s == 10) return (uint32_t)(g.alu >> 16);
    sat_fatal("SCU DSP: D1 source %X", s);
}

static void write_dst(int d, uint32_t v) {
    switch (d) {
    case 0: case 1: case 2: case 3:
        g.ram[d][g.ct[d] & 63] = v;
        g.ct[d] = (g.ct[d] + 1) & 63;
        return;
    case 4: g.rx = (int32_t)v; return;
    case 5: g.p = (int32_t)v; return;               // PL, PH its sign
    case 6: g.ra0 = v; return;
    case 7: g.wa0 = v; return;
    case 10: g.lop = v & 0xFFF; return;
    case 11: g.top = v & 0xFF; return;
    case 12: case 13: case 14: case 15: g.ct[d - 12] = v & 63; return;
    }
    sat_fatal("SCU DSP: destination %X", d);
}

static bool cond(uint32_t c) {
    bool r = false;
    switch (c & 0xF) {
    case 1: r = g.z; break;
    case 2: r = g.s; break;
    case 3: r = g.z || g.s; break;
    case 4: r = g.c; break;
    case 8: r = false; break;                        // T0: DMA is never in progress here
    default: sat_fatal("SCU DSP: condition %02X", c);
    }
    return c & 0x20 ? r : !r;
}

// ---- one operation word ----------------------------------------------------------------------
static void set32(uint32_t r) {
    g.alu = (g.alu & 0xFFFF00000000ull) | r;
    g.z = r == 0;
    g.s = r >> 31;
}

static void op_alu(uint32_t op) {
    uint32_t acl = lo32(g.a), pl = lo32(g.p);
    switch (op >> 26 & 0xF) {
    case 0x0: break;
    case 0x1: set32(acl & pl); g.c = false; break;
    case 0x2: set32(acl | pl); g.c = false; break;
    case 0x3: set32(acl ^ pl); g.c = false; break;
    case 0x4: {
        uint64_t r = (uint64_t)acl + pl;
        set32((uint32_t)r);
        g.c = r >> 32;
        g.v = ((r ^ acl) & (r ^ pl)) >> 31 & 1;
        break;
    }
    case 0x5: {
        uint64_t r = (uint64_t)acl - pl;
        set32((uint32_t)r);
        g.c = r >> 32 & 1;
        g.v = ((acl ^ pl) & (acl ^ (uint32_t)r)) >> 31 & 1;
        break;
    }
    case 0x6: {                                      // AD2: 48-bit A + P
        int64_t r = sext48(g.a + g.p);
        g.c = ((uint64_t)(g.a & 0xFFFFFFFFFFFFll) + (uint64_t)(g.p & 0xFFFFFFFFFFFFll)) >> 48 & 1;
        g.v = ((r ^ g.a) & (r ^ g.p)) >> 47 & 1;
        g.alu = (uint64_t)r & 0xFFFFFFFFFFFFull;
        g.z = g.alu == 0;
        g.s = r < 0;
        break;
    }
    case 0x8: g.c = acl & 1; set32((uint32_t)((int32_t)acl >> 1)); break;             // SR
    case 0x9: g.c = acl & 1; set32(acl >> 1 | acl << 31); break;                        // RR
    case 0xA: g.c = acl >> 31; set32(acl << 1); break;                                  // SL
    case 0xB: g.c = acl >> 31; set32(acl << 1 | acl >> 31); break;                      // RL
    case 0xF: g.c = acl >> 24 & 1; set32(acl << 8 | acl >> 24); break;                  // RL8
    default: sat_fatal("SCU DSP: ALU operation %X", op >> 26 & 0xF);
    }
    int bump[4] = {};
    int64_t mul = g.mul;
    bool loaded = false;
    if (op >> 25 & 1) { g.rx = (int32_t)read_src(op >> 20 & 7, bump); loaded = true; }  // MOV [s],X
    switch (op >> 23 & 3) {
    case 2: g.p = sext48(mul); break;                                                    // MOV MUL,P
    case 3: g.p = (int32_t)read_src(op >> 20 & 7, bump); break;                          // MOV [s],P
    }
    if (op >> 19 & 1) { g.ry = (int32_t)read_src(op >> 14 & 7, bump); loaded = true; }  // MOV [s],Y
    switch (op >> 17 & 3) {
    case 1: g.a = 0; break;                                                              // CLR A
    case 2: g.a = sext48((int64_t)(g.alu << 16) >> 16); break;                           // MOV ALU,A
    case 3: g.a = (int32_t)read_src(op >> 14 & 7, bump); break;                          // MOV [s],A
    }
    for (int b = 0; b < 4; ++b)
        if (bump[b]) g.ct[b] = (g.ct[b] + 1) & 63;
    switch (op >> 12 & 3) {
    case 1: write_dst(op >> 8 & 0xF, (uint32_t)(int32_t)(int8_t)(op & 0xFF)); break;    // MOV SImm,[d]
    case 3: write_dst(op >> 8 & 0xF, read_d1(op & 0xF)); break;                           // MOV [s],[d]
    }
    if (loaded) g.mul = (int64_t)g.rx * g.ry;
}

static void op_dma(uint32_t op) {
    bool hold = op >> 14 & 1, to_d0 = op >> 12 & 1;
    int ram = op >> 8 & 7;
    int bump[4] = {};
    uint32_t n = op >> 13 & 1 ? read_src(op & 7, bump) : op & 0xFF;
    for (int b = 0; b < 4; ++b)
        if (bump[b]) g.ct[b] = (g.ct[b] + 1) & 63;
    // the D0 address steps by 4 bytes a word for add codes 1 and 2 (0: stays),
    // by 16 << ... for the larger ones, as ST-097's table
    static const uint32_t kAdd[8] = {0, 4, 4, 16, 16, 64, 128, 256};
    uint32_t add = kAdd[op >> 15 & 7];
    uint32_t addr = (to_d0 ? g.wa0 : g.ra0) << 2 & 0x07FFFFFFu;
    for (uint32_t k = 0; k < n; ++k) {
        if (!to_d0) {
            uint32_t v = (uint32_t)ld16(addr) << 16 | ld16(addr + 2);
            if (ram == 4) g.prog[(g.pc + k) & 0xFF] = v;
            else if (ram < 4) g.ram[ram][(g.ct[ram] + k) & 63] = v;
            else sat_fatal("SCU DSP: DMA to %d", ram);
        } else {
            if (ram > 3) sat_fatal("SCU DSP: DMA from %d", ram);
            uint32_t v = g.ram[ram][(g.ct[ram] + k) & 63];
            st16(addr, v >> 16);
            st16(addr + 2, v & 0xFFFF);
        }
        addr += add;
    }
    if (!hold) {
        if (to_d0) g.wa0 += n * add / 4;
        else g.ra0 += n * add / 4;
    }
}

static void run() {
    int delay = -1;                                  // a delay slot's address, or -1
    for (long steps = 0; g.ex; ++steps) {
        if (steps > 10000000) sat_fatal("SCU DSP: the program at %02X does not end", g.pc);
        uint32_t op;
        if (delay >= 0) { op = g.prog[delay]; delay = -1; }
        else op = g.prog[g.pc++];
        switch (op >> 30) {
        case 0: op_alu(op); break;
        case 1: sat_fatal("SCU DSP: undefined word %08X", op);
        case 2: {                                    // MVI
            bool doit = op >> 25 & 1 ? cond(op >> 19 & 0x7F) : true;
            if (!doit) break;
            uint32_t v = op >> 25 & 1 ? (uint32_t)((int32_t)(op << 13) >> 13) : (uint32_t)((int32_t)(op << 7) >> 7);
            int d = op >> 26 & 0xF;
            if (d == 12) { delay = g.pc; g.top = g.pc; g.pc = v & 0xFF; }
            else write_dst(d, v);
            break;
        }
        case 3:
            switch (op >> 28 & 3) {
            case 0: op_dma(op); break;
            case 1:                                  // JMP
                if (!(op >> 19 & 0x7F) || cond(op >> 19 & 0x7F)) { delay = g.pc; g.pc = op & 0xFF; }
                break;
            case 2:                                  // BTM / LPS
                if (g.lop) {
                    --g.lop;
                    delay = g.pc;
                    g.pc = op >> 27 & 1 ? g.pc - 1 : g.top;
                }
                break;
            case 3:                                  // END / ENDI
                g.ex = false;
                if (op >> 27 & 1) { g.end = true; scu_raise(5); }
                break;
            }
        }
    }
}

// ---- the registers --------------------------------------------------------------------------
uint32_t scu_dsp_read(uint32_t off) {
    switch (off) {
    case 0x80: {                                     // PPAF: flags and PC; reading clears V and E
        uint32_t v = (uint32_t)g.s << 22 | (uint32_t)g.z << 21 | (uint32_t)g.c << 20 | (uint32_t)g.v << 19 |
                     (uint32_t)g.end << 18 | (uint32_t)g.ex << 16 | g.pc;
        g.v = g.end = false;
        return v;
    }
    case 0x8C: {
        uint32_t v = g.ram[g.ra >> 6 & 3][g.ra & 63];
        g.ra = (g.ra + 1) & 0xFF;
        return v;
    }
    }
    return 0;
}

void scu_dsp_write(uint32_t off, uint32_t v) {
    switch (off) {
    case 0x80:                                       // PPAF
        if (v >> 15 & 1) g.pc = v & 0xFF;            // LE: load the PC
        if (v >> 16 & 1 && !g.ex) {                  // EX: run
            g.ex = true;
            run();
        } else if (!(v >> 16 & 1)) {
            g.ex = false;
        }
        return;
    case 0x84: g.prog[g.pc++] = v; return;           // PPD
    case 0x88: g.ra = v & 0xFF; return;              // PDA
    case 0x8C:                                       // PDD
        g.ram[g.ra >> 6 & 3][g.ra & 63] = v;
        g.ra = (g.ra + 1) & 0xFF;
        return;
    }
}
