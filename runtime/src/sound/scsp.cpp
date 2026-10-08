// saturn-recomp runtime — the SCSP (Yamaha YMF292): the 32 slots, the timers,
// the interrupts to the 68000 and to the SCU, the DMA, the DSP and the
// mixer. One call to scsp_sample makes one stereo sample at 44 100 Hz;
// sound.cpp runs the 68000 between them.
//
// Registers (0x25B00000 on the SH-2, 0x100000 on the 68000), as 16-bit
// words, byte access merged into them:
//   0x000-0x3FF  32 slots of 0x20 bytes: the sample (start, loop, 8/16 bit,
//                source), the envelope, the level, pitch and LFO, FM, the
//                DSP input and the direct and effect sends
//   0x400-0x42F  master volume, the DSP ring buffer, MIDI, the slot monitor
//                (MSLC in, CA/SGC/EG out), DMA, timers A/B/C, the 68000's
//                interrupts (SCIEB/SCIPD/SCIRE/SCILV0-2) and the main CPU's
//                (MCIEB/MCIPD/MCIRE)
//   0x600-0x67F  the sound stack: the last two samples of every slot (FM)
//   0x700-0xEE3  the DSP: COEF, MADRS, MPRO (128 steps), TEMP, MEMS, MIXS,
//                EFREG, EXTS
//
// A slot plays 8- or 16-bit PCM from sound RAM at 44.1 kHz * 2^OCT *
// (1 + FNS/1024), linearly interpolated, with no loop, a loop, a reverse
// loop or an alternating one; the address can be moved by FM (two earlier
// slot outputs from the sound stack, scaled by MDL). Its envelope has
// attack, two decays and release, rates scaled by key (KRS); its pitch and
// level LFOs saw, square, triangle or noise. The output goes, attenuated by
// TL, to the direct mix (DISDL, DIPAN), to the DSP's input MIXS[ISEL]
// (IMXL), and to the sound stack. The DSP's outputs EFREG 0-15 are mixed with
// slot 0-15's EFSDL and EFPAN, the external inputs EXTS 0-1 (CD audio) with
// slot 16-17's.
//
// Timers A, B and C count up once every 2^TxCTL samples from the value
// written, which is loaded at the next count; reaching 0xFF sets their
// interrupt bit (6, 7, 8) for both CPUs. Bit 10 is set every sample. The
// 68000's level is the highest of SCILV2-0 over the bits pending and
// enabled (bits 8-10 take bit 7's level).
//
// The envelope, LFO and DSP follow MAME's scsp.cpp and scspdsp.cpp: the
// envelope times in milliseconds, the key-rate scaling, the LFO tables and
// scales, the FM scaling, and the DSP step with its floating-point
// PACK/UNPACK of the ring buffer. Those parts are derived from MAME's code:
//   Copyright (c) ElSemi, R. Belmont (thanks-to kingshriek), BSD-3-Clause;
//   the licence's text is in THIRD_PARTY.md at the root of saturn-recomp.
// The timers' behaviour (free-running, loaded at the next count) is the
// hardware's as Mednafen documents it. Not done: MIDI (its input reads empty), the DSP's
// ring-buffer writes on even steps.
#include "saturn.h"
#include "state.h"
#include "sound.h"
#include <algorithm>
#include <cmath>
#include <cstring>

uint8_t g_sound_ram[0x80000];

namespace {

constexpr int kShift = 12;                      // fraction bits of a sample position
constexpr int kEgShift = 16;                    // fraction bits of the envelope

enum { EG_ATTACK, EG_DECAY1, EG_DECAY2, EG_RELEASE };

struct Lfo {
    uint32_t phase, step;
    const int* table;
    const int* scale;
};

struct Slot {
    uint16_t r[16];                             // the registers as written
    bool active, keyed, backwards;
    int64_t pos;                                // samples from SA, kShift fraction bits
    int eg_state;
    int32_t eg_vol;                             // 0..0x3FF << kEgShift, 0x3FF loudest
    int32_t ar, d1r, d2r, rr;                   // per-sample steps
    int dl;
    Lfo plfo, alfo;
    // register fields
    bool pcm8() const { return r[0] & 0x10; }
    int sbctl() const { return r[0] >> 9 & 3; }
    int ssctl() const { return r[0] >> 7 & 3; }
    int lpctl() const { return r[0] >> 5 & 3; }
    uint32_t sa() const { return (uint32_t)(r[0] & 0xF) << 16 | r[1]; }
    uint32_t lsa() const { return r[2]; }
    uint32_t lea() const { return r[3]; }
    int d2r_r() const { return r[4] >> 11 & 31; }
    int d1r_r() const { return r[4] >> 6 & 31; }
    bool eghold() const { return r[4] & 0x20; }
    int ar_r() const { return r[4] & 31; }
    bool lpslnk() const { return r[5] & 0x4000; }
    int krs() const { return r[5] >> 10 & 15; }
    int dl_r() const { return r[5] >> 5 & 31; }
    int rr_r() const { return r[5] & 31; }
    bool stwinh() const { return r[6] & 0x200; }
    bool sdir() const { return r[6] & 0x100; }
    int tl() const { return r[6] & 0xFF; }
    int mdl() const { return r[7] >> 12 & 15; }
    int mdxsl() const { return r[7] >> 6 & 63; }
    int mdysl() const { return r[7] & 63; }
    int oct() const { return ((r[8] >> 11 & 15) ^ 8) - 8; }
    int fns() const { return r[8] & 0x3FF; }
    bool lfore() const { return r[9] & 0x8000; }
    int lfof() const { return r[9] >> 10 & 31; }
    int plfows() const { return r[9] >> 8 & 3; }
    int plfos() const { return r[9] >> 5 & 7; }
    int alfows() const { return r[9] >> 3 & 3; }
    int alfos() const { return r[9] & 7; }
    int isel() const { return r[10] >> 3 & 15; }
    int imxl() const { return r[10] & 7; }
    int disdl() const { return r[11] >> 13 & 7; }
    int dipan() const { return r[11] >> 8 & 31; }
    int efsdl() const { return r[11] >> 5 & 7; }
    int efpan() const { return r[11] & 31; }
};

struct Timer { int ctl; int counter; int reload; };

struct Dsp {
    int16_t coef[64];
    uint16_t madrs[32];
    uint16_t mpro[128 * 4];
    int32_t temp[128], mems[32], mixs[16];
    int16_t exts[2], efreg[16];
    uint32_t dec;
    int last_step;                              // steps to run (0: stopped)
};

Slot g_slot[32];
uint16_t g_com[0x18];                           // 0x400-0x42F as written
Timer g_timer[3];
uint32_t g_scieb, g_scipd, g_scilv[3], g_mcieb, g_mcipd;
bool g_main_line;
uint32_t g_samples;                             // since reset
int32_t g_stack[64];                            // the sound stack (0x600): the slots' last outputs
unsigned g_stack_pos;
Dsp g_dsp;
uint32_t g_noise = 1;

// ---- tables ------------------------------------------------------------------------------
float g_eg_gain[1024], g_tl_gain[256], g_sdl_gain[8], g_pan_l[32], g_pan_r[32], g_mvol[16];
int32_t g_ar_step[64], g_dr_step[64];
int g_alfo[4][256], g_plfo[4][256], g_ascale[8][256], g_pscale[8][256];

const double kArMs[64] = {
    1e5, 1e5, 8100.0, 6900.0, 6000.0, 4800.0, 4000.0, 3400.0, 3000.0, 2400.0, 2000.0, 1700.0, 1500.0,
    1200.0, 1000.0, 860.0, 760.0, 600.0, 500.0, 430.0, 380.0, 300.0, 250.0, 220.0, 190.0, 150.0, 130.0, 110.0, 95.0,
    76.0, 63.0, 55.0, 47.0, 38.0, 31.0, 27.0, 24.0, 19.0, 15.0, 13.0, 12.0, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4, 3.0, 2.4,
    2.0, 1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0.0, 0.0};
const double kDrMs[64] = {
    1e5, 1e5, 118200.0, 101300.0, 88600.0, 70900.0, 59100.0, 50700.0, 44300.0, 35500.0, 29600.0, 25300.0, 22200.0,
    17700.0, 14800.0, 12700.0, 11100.0, 8900.0, 7400.0, 6300.0, 5500.0, 4400.0, 3700.0, 3200.0, 2800.0, 2200.0, 1800.0,
    1600.0, 1400.0, 1100.0, 920.0, 790.0, 690.0, 550.0, 460.0, 390.0, 340.0, 270.0, 230.0, 200.0, 170.0, 140.0, 110.0,
    98.0, 85.0, 68.0, 57.0, 49.0, 43.0, 34.0, 28.0, 25.0, 22.0, 18.0, 14.0, 12.0, 11.0, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1};
const float kLfoHz[32] = {0.17f, 0.19f, 0.23f, 0.27f, 0.34f, 0.39f, 0.45f, 0.55f, 0.68f, 0.78f, 0.92f,
                          1.10f, 1.39f, 1.60f, 1.87f, 2.27f, 2.87f, 3.31f, 3.92f, 4.79f, 6.15f, 7.18f,
                          8.60f, 10.8f, 14.4f, 17.2f, 21.5f, 28.7f, 43.1f, 57.4f, 86.1f, 172.3f};
const float kAScaleDb[8] = {0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f};
const float kPScaleCents[8] = {0.0f, 7.0f, 13.5f, 27.0f, 55.0f, 112.0f, 230.0f, 494.0f};

float db(float d) { return std::pow(10.0f, d / 20.0f); }

void init_tables() {
    for (int i = 0; i < 1024; ++i) g_eg_gain[i] = db(3.0f * (i - 0x3FF) / 32.0f);
    for (int i = 0; i < 256; ++i) {
        static const float kBits[8] = {0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f, 48.0f};
        float d = 0;
        for (int b = 0; b < 8; ++b)
            if (i >> b & 1) d -= kBits[b];
        g_tl_gain[i] = db(d);
    }
    g_sdl_gain[0] = 0;
    for (int i = 1; i < 8; ++i) g_sdl_gain[i] = db(-6.0f * (7 - i));
    for (int p = 0; p < 32; ++p) {
        int a = p & 15;
        float g = a == 15 ? 0.0f : db(-(3.0f * (a & 1) + 6.0f * (a >> 1 & 1) + 12.0f * (a >> 2 & 1) + 24.0f * (a >> 3 & 1)));
        g_pan_l[p] = p < 16 ? g : 1.0f;         // 0x00-0x0F attenuate the left, 0x10-0x1F the right
        g_pan_r[p] = p < 16 ? 1.0f : g;
    }
    g_mvol[0] = 0;
    for (int i = 1; i < 16; ++i) g_mvol[i] = db(-3.0f * (15 - i));
    for (int i = 0; i < 64; ++i) {
        g_ar_step[i] = i < 2 ? 0 : kArMs[i] == 0.0 ? 1024 << kEgShift
                                                   : (int32_t)(1023 * 1000.0 / (44100.0 * kArMs[i]) * (1 << kEgShift));
        g_dr_step[i] = i < 2 ? 0 : (int32_t)(1023 * 1000.0 / (44100.0 * kDrMs[i]) * (1 << kEgShift));
    }
    uint32_t rnd = 0x12345678;
    for (int i = 0; i < 256; ++i) {
        g_alfo[0][i] = 255 - i;                 // saw
        g_plfo[0][i] = i < 128 ? i : i - 256;
        g_alfo[1][i] = i < 128 ? 255 : 0;       // square
        g_plfo[1][i] = i < 128 ? 127 : -128;
        g_alfo[2][i] = i < 128 ? 255 - i * 2 : i * 2 - 256;   // triangle
        g_plfo[2][i] = i < 64 ? i * 2 : i < 128 ? 255 - i * 2 : i < 192 ? 256 - i * 2 : i * 2 - 511;
        rnd = rnd * 1103515245u + 12345u;       // noise
        int a = (int)(rnd >> 16 & 0xFF);
        g_alfo[3][i] = a;
        g_plfo[3][i] = 128 - a;
    }
    for (int s = 0; s < 8; ++s)
        for (int i = 0; i < 256; ++i) {
            g_pscale[s][i] = (int)(256.0f * std::pow(2.0f, kPScaleCents[s] * (i - 128) / 128.0f / 1200.0f));
            g_ascale[s][i] = (int)(256.0f * db(-kAScaleDb[s] * i / 256.0f));
        }
}

// ---- a slot ------------------------------------------------------------------------------
uint32_t step_of(const Slot& s) {
    uint32_t fn = (uint32_t)s.fns() + 1024;
    int sh = s.oct() + kShift - 10;
    return sh >= 0 ? fn << sh : fn >> -sh;
}

void compute_eg(Slot& s) {
    int base = s.krs() == 15 ? 0 : s.oct() + 2 * s.krs() + (s.fns() >> 9 & 1);
    auto rate = [&](int r, const int32_t* table) { return r ? table[std::clamp(base + 2 * r, 0, 63)] : 0; };
    s.ar = rate(s.ar_r(), g_ar_step);
    s.d1r = rate(s.d1r_r(), g_dr_step);
    s.d2r = rate(s.d2r_r(), g_dr_step);
    s.rr = rate(s.rr_r(), g_dr_step);
    s.dl = 0x1F - s.dl_r();
}

void compute_lfo(Slot& s) {
    uint32_t step = (uint32_t)(kLfoHz[s.lfof()] * 256.0f / 44100.0f * 256.0f);
    s.plfo.step = s.alfo.step = step;
    s.plfo.table = g_plfo[s.plfows()];
    s.plfo.scale = g_pscale[s.plfos()];
    s.alfo.table = g_alfo[s.alfows()];
    s.alfo.scale = g_ascale[s.alfos()];
}

void key_on(Slot& s) {
    s.active = true;
    s.pos = 0;
    s.backwards = false;
    compute_eg(s);
    compute_lfo(s);
    s.plfo.phase = s.alfo.phase = 0;
    s.eg_state = EG_ATTACK;
    s.eg_vol = 0x17F << kEgShift;               // MAME's starting point for the attack
}

void key_off(Slot& s) {
    if (s.active) s.eg_state = EG_RELEASE;
}

// The envelope, one sample on: the gain to apply
float eg_step(Slot& s) {
    switch (s.eg_state) {
    case EG_ATTACK:
        s.eg_vol += s.ar;
        if (s.eg_vol >= 0x3FF << kEgShift) {
            s.eg_vol = 0x3FF << kEgShift;
            if (!s.lpslnk()) s.eg_state = s.d1r >= 1024 << kEgShift ? EG_DECAY2 : EG_DECAY1;
        }
        if (s.eghold()) return 1.0f;
        return (float)(s.eg_vol >> kEgShift) / 1024.0f;   // the attack is linear in amplitude
    case EG_DECAY1:
        s.eg_vol = std::max(0, s.eg_vol - s.d1r);
        if (s.eg_vol >> (kEgShift + 5) <= s.dl) s.eg_state = EG_DECAY2;
        break;
    case EG_DECAY2:
        s.eg_vol = std::max(0, s.eg_vol - s.d2r);
        break;
    case EG_RELEASE:
        s.eg_vol -= s.rr;
        if (s.eg_vol <= 0) { s.eg_vol = 0; s.active = false; }
        break;
    }
    return g_eg_gain[s.eg_vol >> kEgShift];
}

int lfo_next(Lfo& l, bool reset) {
    if (reset) l.phase = 0;
    else l.phase += l.step;
    return l.table[l.phase >> 8 & 0xFF];
}

int32_t read_sample(const Slot& s, int64_t idx) {
    if (s.ssctl() == 1) {                       // noise
        g_noise = g_noise * 1103515245u + 12345u;
        return (int16_t)(g_noise >> 16);
    }
    if (s.ssctl() != 0) return 0;
    int32_t v;
    if (s.pcm8()) v = (int8_t)g_sound_ram[(s.sa() + (uint32_t)idx) & 0x7FFFF] << 8;
    else {
        uint32_t a = (s.sa() + 2 * (uint32_t)idx) & 0x7FFFE;
        v = (int16_t)(g_sound_ram[a] << 8 | g_sound_ram[a + 1]);
    }
    if (s.sbctl() & 1) v ^= 0x7FFF;
    if (s.sbctl() & 2) v = (int16_t)(v ^ 0x8000);
    return v;
}

// One sample of a slot, after the envelope and the level LFO, before TL
int32_t slot_sample(Slot& s) {
    int64_t lsa = (int64_t)s.lsa() << kShift, lea = (int64_t)s.lea() << kShift;
    int64_t idx = s.pos >> kShift;
    if (s.mdl() >= 5) {                         // FM: the address moved by two earlier outputs
        int32_t m = (g_stack[(g_stack_pos + s.mdxsl()) & 63] + g_stack[(g_stack_pos + s.mdysl()) & 63]) / 2;
        idx += (m << 10) >> (0x1A - s.mdl());
    }
    int32_t frac = (int32_t)(s.pos & ((1 << kShift) - 1));
    int32_t a = read_sample(s, idx), b = read_sample(s, idx + (s.backwards ? -1 : 1));
    int32_t v = (int32_t)(((int64_t)a * ((1 << kShift) - frac) + (int64_t)b * frac) >> kShift);

    int64_t step = step_of(s);
    if (s.plfos()) step = step * s.plfo.scale[lfo_next(s.plfo, s.lfore()) + 128] / 256;
    s.pos += s.backwards ? -step : step;
    if (!s.backwards && s.pos >= lsa && s.lpslnk() && s.eg_state == EG_ATTACK) s.eg_state = EG_DECAY1;
    switch (s.lpctl()) {
    case 0:                                     // no loop: stop at the end
        if (s.pos >= lea && s.pos >= lsa) s.active = false;
        break;
    case 1:                                     // loop LSA-LEA
        if (s.pos >= lea) s.pos -= std::max<int64_t>(lea - lsa, 1 << kShift);
        break;
    case 2:                                     // reverse: to LSA, then LEA down to LSA, again
        if (!s.backwards && s.pos >= lsa) { s.pos = lea - (s.pos - lsa); s.backwards = true; }
        else if (s.backwards && s.pos < lsa) s.pos = lea - (lsa - s.pos);
        break;
    case 3:                                     // alternating: LEA down to LSA, up to LEA
        if (!s.backwards && s.pos >= lea) { s.pos = 2 * lea - s.pos; s.backwards = true; }
        else if (s.backwards && s.pos < lsa) { s.pos = 2 * lsa - s.pos; s.backwards = false; }
        break;
    }
    if (s.sdir()) return v;                     // direct: no envelope, no LFO
    if (s.alfos()) v = v * s.alfo.scale[lfo_next(s.alfo, s.lfore())] / 256;
    return (int32_t)(v * eg_step(s));
}

// ---- the DSP (after MAME's scspdsp.cpp) -----------------------------------------------------
int32_t sext(int32_t v, int bits) { return (int32_t)((uint32_t)v << (32 - bits)) >> (32 - bits); }

uint16_t dsp_pack(int32_t val) {
    int sign = val >> 23 & 1;
    uint32_t t = (uint32_t)(val ^ (val << 1)) & 0xFFFFFF;
    int e = 0;
    while (e < 12 && !(t & 0x800000)) { t <<= 1; ++e; }
    val = e < 12 ? (val << e) & 0x3FFFFF : val << 11;
    val >>= 11;
    return (uint16_t)((val & 0x7FF) | sign << 15 | e << 11);
}

int32_t dsp_unpack(uint16_t v) {
    int sign = v >> 15 & 1, e = v >> 11 & 15;
    int32_t u = (v & 0x7FF) << 11;
    if (e > 11) { e = 11; u |= sign << 22; }
    else u |= (sign ^ 1) << 22;
    u |= sign << 23;
    return sext(u, 24) >> e;
}

uint32_t g_rbl = 8192, g_rbp;                   // ring buffer: length in words, start in 8 KB units

void dsp_step() {
    Dsp& d = g_dsp;
    std::memset(d.efreg, 0, sizeof d.efreg);
    if (!d.last_step) { std::memset(d.mixs, 0, sizeof d.mixs); return; }
    int32_t acc = 0, memval = 0, frc = 0, yreg = 0;
    uint32_t adrs = 0;
    for (int st = 0; st < d.last_step; ++st) {
        const uint16_t* ip = d.mpro + st * 4;
        uint32_t tra = ip[0] >> 8 & 0x7F, twt = ip[0] >> 7 & 1, twa = ip[0] & 0x7F;
        uint32_t xsel = ip[1] >> 15 & 1, ysel = ip[1] >> 13 & 3, ira = ip[1] >> 6 & 0x3F, iwt = ip[1] >> 5 & 1,
                 iwa = ip[1] & 0x1F;
        uint32_t table = ip[2] >> 15 & 1, mwt = ip[2] >> 14 & 1, mrd = ip[2] >> 13 & 1, ewt = ip[2] >> 12 & 1,
                 ewa = ip[2] >> 8 & 15, adrl = ip[2] >> 7 & 1, frcl = ip[2] >> 6 & 1, shift = ip[2] >> 4 & 3,
                 yrl = ip[2] >> 3 & 1, negb = ip[2] >> 2 & 1, zero = ip[2] >> 1 & 1, bsel = ip[2] & 1;
        uint32_t nofl = ip[3] >> 15 & 1, coef = ip[3] >> 9 & 0x3F, masa = ip[3] >> 2 & 0x1F, adreb = ip[3] >> 1 & 1,
                 nxadr = ip[3] & 1;
        int32_t in;
        if (ira <= 0x1F) in = d.mems[ira];
        else if (ira <= 0x2F) in = d.mixs[ira - 0x20] << 4;
        else if (ira <= 0x31) in = d.exts[ira - 0x30] << 8;
        else break;
        in = sext(in, 24);
        if (iwt) {
            d.mems[iwa] = memval;
            if (ira == iwa) in = memval;
        }
        int32_t b = 0;
        if (!zero) {
            b = bsel ? acc : sext(d.temp[(tra + d.dec) & 0x7F], 24);
            if (negb) b = -b;
        }
        int32_t x = xsel ? in : sext(d.temp[(tra + d.dec) & 0x7F], 24);
        int32_t y = ysel == 0 ? frc : ysel == 1 ? d.coef[coef] >> 3 : ysel == 2 ? (yreg >> 11) & 0x1FFF : (yreg >> 4) & 0x0FFF;
        if (yrl) yreg = in;
        int32_t shifted = shift == 0 ? std::clamp(acc, -0x800000, 0x7FFFFF)
                        : shift == 1 ? std::clamp(acc * 2, -0x800000, 0x7FFFFF)
                        : shift == 2 ? sext(acc * 2, 24) : sext(acc, 24);
        acc = (int32_t)(((int64_t)x * sext(y, 13)) >> 12) + b;
        if (twt) d.temp[(twa + d.dec) & 0x7F] = shifted;
        if (frcl) frc = shift == 3 ? shifted & 0x0FFF : (shifted >> 11) & 0x1FFF;
        if ((mrd || mwt) && (st & 1)) {
            uint32_t addr = d.madrs[masa];
            if (!table) addr += d.dec;
            if (adreb) addr += adrs & 0x0FFF;
            if (nxadr) ++addr;
            addr &= table ? 0xFFFF : g_rbl - 1;
            addr = ((addr + (g_rbp << 12)) << 1) & 0x7FFFE;
            if (mrd) {
                uint16_t w = (uint16_t)(g_sound_ram[addr] << 8 | g_sound_ram[addr + 1]);
                memval = nofl ? w << 8 : dsp_unpack(w);
            }
            if (mwt) {
                uint16_t w = nofl ? (uint16_t)(shifted >> 8) : dsp_pack(shifted);
                g_sound_ram[addr] = (uint8_t)(w >> 8);
                g_sound_ram[addr + 1] = (uint8_t)w;
            }
        }
        if (adrl) adrs = shift == 3 ? (shifted >> 12) & 0xFFF : (uint32_t)(in >> 16);
        if (ewt) d.efreg[ewa] = (int16_t)(d.efreg[ewa] + (shifted >> 8));
    }
    --d.dec;
    std::memset(d.mixs, 0, sizeof d.mixs);
}

void dsp_program_changed() {
    int i = 127;
    while (i >= 0 && !(g_dsp.mpro[i * 4] | g_dsp.mpro[i * 4 + 1] | g_dsp.mpro[i * 4 + 2] | g_dsp.mpro[i * 4 + 3])) --i;
    g_dsp.last_step = i + 1;
}

// ---- interrupts ---------------------------------------------------------------------------
int g_cpu_level;

void irq_update() {
    uint32_t m = g_scipd & g_scieb;
    if (m & ~0xFFu) m = (m & 0xFF) | 0x80;      // bits 8-10 take bit 7's level
    int level = 0;
    for (int b = 0; b < 8; ++b)
        if (m >> b & 1)
            level = std::max(level, (int)((g_scilv[0] >> b & 1) | (g_scilv[1] >> b & 1) << 1 | (g_scilv[2] >> b & 1) << 2));
    if (level != g_cpu_level) {
        g_cpu_level = level;
        sound_irq_changed();
    }
    bool main = g_mcipd & g_mcieb;
    if (main && !g_main_line) sound_main_irq();
    g_main_line = main;
}

void pend(uint32_t bits) {
    g_scipd |= bits;
    g_mcipd |= bits;
}

// ---- registers ------------------------------------------------------------------------------
uint16_t monitor() {                            // 0x408: the slot MSLC names, CA, SGC, EG
    const Slot& s = g_slot[g_com[4] >> 11 & 31];
    uint32_t ca = (uint32_t)(s.pos >> (kShift + 12)) & 0xF;
    uint32_t sgc = s.active ? (uint32_t)s.eg_state : EG_RELEASE;
    uint32_t eg = (0x1F - (uint32_t)(s.eg_vol >> (kEgShift + 5))) & 0x1F;
    return (uint16_t)((g_com[4] & 0xF800) | ca << 7 | sgc << 5 | eg);
}

void dma() {
    uint32_t mem = ((uint32_t)(g_com[0x14 / 2] & 0xF000) << 4 | (g_com[0x12 / 2] & 0xFFFE)) & 0x7FFFE;
    uint32_t reg = g_com[0x14 / 2] & 0x0FFE, len = g_com[0x16 / 2] & 0x0FFE;
    bool to_mem = g_com[0x16 / 2] & 0x2000, gate = g_com[0x16 / 2] & 0x4000;
    sat_trace("SCSP DMA %s %05X <-> %03X, %u bytes%s", to_mem ? "reg->mem" : "mem->reg", mem, reg, len, gate ? ", zeros" : "");
    for (uint32_t i = 0; i < len; i += 2) {
        if (to_mem) {
            uint16_t v = gate ? 0 : (uint16_t)scsp_read((reg + i) & 0xFFE, 2);
            g_sound_ram[(mem + i) & 0x7FFFF] = (uint8_t)(v >> 8);
            g_sound_ram[(mem + i + 1) & 0x7FFFF] = (uint8_t)v;
        } else {
            uint32_t a = (mem + i) & 0x7FFFE;
            scsp_write((reg + i) & 0xFFE, gate ? 0 : (uint32_t)(g_sound_ram[a] << 8 | g_sound_ram[a + 1]), 2);
        }
    }
    g_com[0x16 / 2] &= ~0x1000;
    pend(0x10);
    irq_update();
}

uint16_t read16(uint32_t off) {
    if (off < 0x400) {
        const Slot& s = g_slot[off >> 5];
        int w = off >> 1 & 15;
        return w == 0 ? s.r[0] & ~0x1000 : w < 12 ? s.r[w] : 0;
    }
    if (off < 0x430) {
        switch (off) {
        case 0x404: return 0x0900;              // MIDI: output empty, input empty
        case 0x408: return monitor();
        case 0x418: case 0x41A: case 0x41C: return 0;
        case 0x41E: return (uint16_t)g_scieb;
        case 0x420: return (uint16_t)g_scipd;
        case 0x42A: return (uint16_t)g_mcieb;
        case 0x42C: return (uint16_t)g_mcipd;
        case 0x422: case 0x424: case 0x426: case 0x428: case 0x42E: return 0;
        }
        return g_com[(off - 0x400) >> 1];
    }
    if (off >= 0x600 && off < 0x680) return (uint16_t)g_stack[(off - 0x600) >> 1];
    if (off >= 0x700 && off < 0x780) return (uint16_t)g_dsp.coef[(off - 0x700) >> 1];
    if (off >= 0x780 && off < 0x800) return g_dsp.madrs[(off - 0x780) >> 1 & 31];
    if (off >= 0x800 && off < 0xC00) return g_dsp.mpro[(off - 0x800) >> 1];
    auto half = [&](int32_t v) { return (uint16_t)(off & 2 ? v & 0xFFFF : (uint32_t)v >> 16); };
    if (off >= 0xC00 && off < 0xE00) return half(g_dsp.temp[(off >> 2) & 0x7F]);
    if (off >= 0xE00 && off < 0xE80) return half(g_dsp.mems[(off >> 2) & 0x1F]);
    if (off >= 0xE80 && off < 0xEC0) return half(g_dsp.mixs[(off >> 2) & 0xF]);
    if (off >= 0xEC0 && off < 0xEE0) return (uint16_t)g_dsp.efreg[(off - 0xEC0) >> 1];
    if (off >= 0xEE0 && off < 0xEE4) return (uint16_t)g_dsp.exts[(off - 0xEE0) >> 1];
    return 0;
}

// A 16-bit write; mask says which bytes the CPU wrote
void write16(uint32_t off, uint16_t v, uint16_t mask) {
    if (off < 0x400) {
        Slot& s = g_slot[off >> 5];
        int w = off >> 1 & 15;
        if (w >= 12) return;
        s.r[w] = (uint16_t)((s.r[w] & ~mask) | (v & mask));
        if (w == 0 && (s.r[0] & 0x1000)) {      // KYONEX: every slot takes its KYONB
            s.r[0] &= ~0x1000;
            for (Slot& t : g_slot) {
                bool kb = t.r[0] & 0x0800;
                if (kb && !t.keyed) {
                    key_on(t);
                    sat_trace("SCSP key on %2d: SA %05X LSA %04X LEA %04X LP %d %s OCT %d FNS %03X TL %02X DISDL %d DIPAN %02X "
                              "IMXL %d ISEL %d EFSDL %d EFPAN %02X AR %d D1R %d D2R %d RR %d DL %d MDL %d", (int)(&t - g_slot),
                              t.sa(), t.lsa(), t.lea(), t.lpctl(), t.pcm8() ? "8" : "16", t.oct(), t.fns(), t.tl(), t.disdl(),
                              t.dipan(), t.imxl(), t.isel(), t.efsdl(), t.efpan(), t.ar_r(), t.d1r_r(), t.d2r_r(), t.rr_r(),
                              t.dl_r(), t.mdl());
                }
                else if (!kb && t.keyed) key_off(t);
                t.keyed = kb;
            }
        }
        if (w == 8 || w == 4 || w == 5) compute_eg(s);   // pitch moves key scaling; rates
        if (w == 9) compute_lfo(s);
        return;
    }
    if (off < 0x430) {
        uint16_t& r = g_com[(off - 0x400) >> 1];
        uint16_t nv = (uint16_t)((r & ~mask) | (v & mask));
        switch (off) {
        case 0x402: r = nv; g_rbl = 8192u << (nv >> 7 & 3); g_rbp = nv & 0x3F; return;
        case 0x408: r = nv & 0xF800; return;
        case 0x416: r = nv; if (nv & 0x1000) dma(); return;
        case 0x418: case 0x41A: case 0x41C: {
            Timer& t = g_timer[(off - 0x418) >> 1];
            if (mask & 0xFF00) t.ctl = v >> 8 & 7;
            if (mask & 0x00FF) t.reload = v & 0xFF;
            return;
        }
        case 0x41E: g_scieb = nv & 0x7FF; irq_update(); return;
        case 0x420: g_scipd |= v & mask & 0x20; irq_update(); return;    // bit 5: the CPU's own
        case 0x422: g_scipd &= ~(uint32_t)(v & mask); irq_update(); return;
        case 0x424: case 0x426: case 0x428: {
            uint32_t& l = g_scilv[(off - 0x424) >> 1];
            l = ((l & ~mask) | (v & mask)) & 0xFF;
            irq_update();
            return;
        }
        case 0x42A: g_mcieb = nv & 0x7FF; irq_update(); return;
        case 0x42C: g_mcipd |= v & mask & 0x20; irq_update(); return;
        case 0x42E: g_mcipd &= ~(uint32_t)(v & mask); irq_update(); return;
        }
        r = nv;
        return;
    }
    auto merge = [&](uint16_t old) { return (uint16_t)((old & ~mask) | (v & mask)); };
    if (off >= 0x600 && off < 0x680) { int32_t& x = g_stack[(off - 0x600) >> 1]; x = (int16_t)merge((uint16_t)x); return; }
    if (off >= 0x700 && off < 0x780) { int16_t& x = g_dsp.coef[(off - 0x700) >> 1]; x = (int16_t)merge((uint16_t)x); return; }
    if (off >= 0x780 && off < 0x800) { uint16_t& x = g_dsp.madrs[(off - 0x780) >> 1 & 31]; x = merge(x); return; }
    if (off >= 0x800 && off < 0xC00) {
        uint16_t& x = g_dsp.mpro[(off - 0x800) >> 1];
        x = merge(x);
        dsp_program_changed();
        return;
    }
    auto half = [&](int32_t& x) {
        uint32_t u = (uint32_t)x;
        u = off & 2 ? (u & 0xFFFF0000u) | merge((uint16_t)u) : (u & 0xFFFF) | (uint32_t)merge((uint16_t)(u >> 16)) << 16;
        x = (int32_t)u;
    };
    if (off >= 0xC00 && off < 0xE00) { half(g_dsp.temp[(off >> 2) & 0x7F]); return; }
    if (off >= 0xE00 && off < 0xE80) { half(g_dsp.mems[(off >> 2) & 0x1F]); return; }
    if (off >= 0xE80 && off < 0xEC0) { half(g_dsp.mixs[(off >> 2) & 0xF]); return; }
    if (off >= 0xEC0 && off < 0xEE0) { int16_t& x = g_dsp.efreg[(off - 0xEC0) >> 1]; x = (int16_t)merge((uint16_t)x); return; }
}

}   // namespace

// ---- the interface ------------------------------------------------------------------------------
void scsp_reset() {
    static bool tables;
    if (!tables) { init_tables(); tables = true; }
    for (Slot& s : g_slot) { s = Slot{}; compute_lfo(s); }
    std::memset(g_com, 0, sizeof g_com);
    for (Timer& t : g_timer) t = Timer{0, 0, -1};
    g_scieb = g_scipd = g_mcieb = g_mcipd = 0;
    g_scilv[0] = g_scilv[1] = g_scilv[2] = 0;
    g_samples = 0;
    std::memset(g_stack, 0, sizeof g_stack);
    g_dsp = Dsp{};
    g_rbl = 8192;
    g_rbp = 0;
    g_cpu_level = 0;
    g_main_line = false;
}

uint32_t scsp_read(uint32_t off, int size) {
    off &= 0xFFF;
    if (size == 4) return (uint32_t)read16(off) << 16 | read16(off + 2);
    uint16_t w = read16(off & ~1u);
    if (size == 2) return w;
    return off & 1 ? w & 0xFF : w >> 8;
}

void scsp_write(uint32_t off, uint32_t v, int size) {
    off &= 0xFFF;
    if (size == 4) { write16(off, (uint16_t)(v >> 16), 0xFFFF); write16(off + 2, (uint16_t)v, 0xFFFF); return; }
    if (size == 2) { write16(off, (uint16_t)v, 0xFFFF); return; }
    if (off & 1) write16(off & ~1u, (uint16_t)(v & 0xFF), 0x00FF);
    else write16(off, (uint16_t)(v << 8), 0xFF00);
}

int scsp_cpu_level() { return g_cpu_level; }

uint32_t scsp_active_slots() {
    uint32_t m = 0;
    for (int i = 0; i < 32; ++i)
        if (g_slot[i].active) m |= 1u << i;
    return m;
}

void scsp_sample(int16_t exts_l, int16_t exts_r, int16_t out[2]) {
    // the timers, then this sample's interrupt
    for (Timer& t : g_timer) {
        if (g_samples & ((1u << t.ctl) - 1)) continue;
        if (t.reload >= 0) { t.counter = t.reload; t.reload = -1; }
        else t.counter = (t.counter + 1) & 0xFF;
        if (t.counter == 0xFF) pend(0x40u << (&t - g_timer));
    }
    pend(0x400);
    irq_update();
    ++g_samples;

    float l = 0, r = 0;
    for (Slot& s : g_slot) {
        int32_t stacked = 0;
        if (s.active) {
            int32_t v = slot_sample(s);
            float tl = g_tl_gain[s.tl()];
            float lv = (float)v * tl;
            l += lv * g_sdl_gain[s.disdl()] * g_pan_l[s.dipan()];
            r += lv * g_sdl_gain[s.disdl()] * g_pan_r[s.dipan()];
            if (s.imxl()) g_dsp.mixs[s.isel()] += (int32_t)(lv * g_sdl_gain[s.imxl()] * 16.0f);   // 20 bits
            stacked = (int32_t)(2.0f * (s.sdir() ? (float)v : lv));   // MAME's scale for FM
        }
        if (!s.stwinh()) g_stack[g_stack_pos] = stacked;
        g_stack_pos = (g_stack_pos + 1) & 63;
    }
    g_dsp.exts[0] = exts_l;
    g_dsp.exts[1] = exts_r;
    dsp_step();
    for (int i = 0; i < 16; ++i) {
        const Slot& s = g_slot[i];
        if (!s.efsdl()) continue;
        l += g_dsp.efreg[i] * g_sdl_gain[s.efsdl()] * g_pan_l[s.efpan()];
        r += g_dsp.efreg[i] * g_sdl_gain[s.efsdl()] * g_pan_r[s.efpan()];
    }
    for (int i = 0; i < 2; ++i) {
        const Slot& s = g_slot[16 + i];
        if (!s.efsdl()) continue;
        l += g_dsp.exts[i] * g_sdl_gain[s.efsdl()] * g_pan_l[s.efpan()];
        r += g_dsp.exts[i] * g_sdl_gain[s.efsdl()] * g_pan_r[s.efpan()];
    }
    float mv = g_mvol[g_com[0] & 15];
    out[0] = (int16_t)std::clamp((int32_t)(l * mv), -32768, 32767);
    out[1] = (int16_t)std::clamp((int32_t)(r * mv), -32768, 32767);
}

// An LFO's tables by their number among the tables of their kind.
template <size_t N>
static void lfo_table(State& s, const int*& p, int (&tables)[N][256]) {
    int8_t k = -1;
    for (size_t i = 0; i < N; ++i)
        if (p == tables[i]) k = (int8_t)i;
    s(k);
    if (s.loading) p = k < 0 ? nullptr : tables[k];
}

void scsp_state(State& s) {
    s(g_sound_ram);
    for (Slot& sl : g_slot) {
        s(sl.r), s(sl.active), s(sl.keyed), s(sl.backwards), s(sl.pos), s(sl.eg_state), s(sl.eg_vol);
        s(sl.ar), s(sl.d1r), s(sl.d2r), s(sl.rr), s(sl.dl);
        for (Lfo* l : {&sl.plfo, &sl.alfo}) s(l->phase), s(l->step);
        lfo_table(s, sl.plfo.table, g_plfo), lfo_table(s, sl.plfo.scale, g_pscale);
        lfo_table(s, sl.alfo.table, g_alfo), lfo_table(s, sl.alfo.scale, g_ascale);
    }
    s(g_com), s(g_timer), s(g_scieb), s(g_scipd), s(g_scilv), s(g_mcieb), s(g_mcipd), s(g_main_line);
    s(g_samples), s(g_stack), s(g_stack_pos), s(g_dsp), s(g_noise), s(g_rbl), s(g_rbp), s(g_cpu_level);
}
