// saturn-recomp runtime — the sound side's CPU and its pace: the 68000 that runs
// the driver the game loads into sound RAM, the SCSP beside it, the SH-2's
// view of both, and where the samples go.
//
// The 68000 is Musashi (third_party/musashi, MIT), a plain 68000 at 11.2896
// MHz: 256 of its cycles to each of the SCSP's samples. SMPC's SNDON resets
// it (the stack pointer and PC from sound RAM 0 and 4, where the game has
// put its driver) and lets it run, SNDOFF holds it. Its bus: sound RAM at
// 0x000000-0x0FFFFF (512 KB, mirrored), the SCSP's registers at 0x100000.
// Its interrupts are the SCSP's, autovectored, at the level SCILV gives.
//
// Time is the machine's (virtual unless --realtime): at each of the master's
// polls, sound_tick makes the samples due since the last one, and between
// two samples the 68000 runs 256 cycles. The SH-2 and the 68000 meet at
// the poll's grain (about 100 us), which is how a driver's handshake sees
// it on the Saturn too: through sound RAM, at no fixed moment.
//
// The CD block's audio (cdblock.cpp) comes in at the SCSP's external input,
// EXTS 0-1, a sample for a sample.
//
// The samples go to the window's audio stream (host.cpp) and, with --wav,
// to a 16-bit stereo WAV file of the whole run (--wav, or --video's).
#include "saturn.h"
#include "sound.h"
#include "host.h"
#include "video.h"
#include <cstdio>
#include <vector>
extern "C" {
#include "m68k.h"
}

static bool g_on;                                // SNDON: the 68000 runs
static bool g_audio;                             // a host device takes the samples
static uint64_t g_samples;                       // made since power-on
static int g_debt;                               // 68000 cycles owed to the current sample
static std::vector<int16_t> g_out;               // made this poll, interleaved
static FILE* g_wav;
static uint64_t g_wav_frames;
static uint64_t g_peak_frames;                   // samples not silent: the stop report

// ---- the 68000's bus -----------------------------------------------------------------------------
static uint32_t bus_read(uint32_t a, int size) {
    a &= 0xFFFFFF;
    if (a < 0x100000) {
        a &= 0x7FFFF;
        uint32_t v = 0;
        for (int i = 0; i < size; ++i) v = v << 8 | g_sound_ram[(a + i) & 0x7FFFF];
        return v;
    }
    if (a < 0x200000) return scsp_read(a & 0xFFF, size);
    return 0;
}

static void bus_write(uint32_t a, uint32_t v, int size) {
    a &= 0xFFFFFF;
    if (a < 0x100000) {
        a &= 0x7FFFF;
        for (int i = size - 1; i >= 0; --i, v >>= 8) g_sound_ram[(a + i) & 0x7FFFF] = (uint8_t)v;
        return;
    }
    if (a < 0x200000) scsp_write(a & 0xFFF, v, size);
}

extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return bus_read(a, 1); }
unsigned int m68k_read_memory_16(unsigned int a) { return bus_read(a, 2); }
unsigned int m68k_read_memory_32(unsigned int a) { return bus_read(a, 4); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return bus_read(a, 1); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return bus_read(a, 2); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return bus_read(a, 4); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { bus_write(a, v, 1); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { bus_write(a, v, 2); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { bus_write(a, v, 4); }
}

void sound_irq_changed() {
    if (g_on) m68k_set_irq((unsigned)scsp_cpu_level());
}

void sound_main_irq() { scu_raise(IRQ_SOUND_REQ); }

// ---- power and pace --------------------------------------------------------------------------
static void wav_header() {
    uint32_t data = (uint32_t)(g_wav_frames * 4);
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                     0x44, 0xAC, 0, 0, 0x10, 0xB1, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    uint32_t riff = data + 36;
    for (int i = 0; i < 4; ++i) { h[4 + i] = (uint8_t)(riff >> (8 * i)); h[40 + i] = (uint8_t)(data >> (8 * i)); }
    std::fseek(g_wav, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof h, g_wav);
    std::fseek(g_wav, 0, SEEK_END);
}

void sound_init() {
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    scsp_reset();
    std::string wav = movie_on() ? movie_wav() : g_cfg.wav;
    if (!wav.empty()) {
        g_wav = std::fopen(wav.c_str(), "wb");
        if (!g_wav) sat_fatal("cannot write %s", wav.c_str());
        wav_header();
    }
    g_audio = host_audio_open();
}

void sound_power(bool on) {
    if (on && !g_on) {
        m68k_pulse_reset();
        g_debt = 0;
        sat_trace("68000 reset: SSP %06X, PC %06X", m68k_get_reg(nullptr, M68K_REG_SP), m68k_get_reg(nullptr, M68K_REG_PC));
    }
    g_on = on;
    if (on) m68k_set_irq((unsigned)scsp_cpu_level());
}

void sound_tick() {
    uint64_t due = sat_now() * 441 / 10000000;   // samples at 44 100 Hz
    while (g_samples < due) {
        if (g_on) {
            g_debt += 256;
            if (g_debt > 0) g_debt -= m68k_execute(g_debt);
        }
        int16_t cd[2], s[2];
        cd_audio_sample(cd);
        scsp_sample(cd[0], cd[1], s);
        if (s[0] | s[1]) ++g_peak_frames;
        g_out.push_back(s[0]);
        g_out.push_back(s[1]);
        ++g_samples;
    }
    if (g_out.empty()) return;
    if (g_audio) host_audio_push(g_out.data(), (int)g_out.size() / 2);
    if (g_wav) {
        std::vector<uint8_t> le(g_out.size() * 2);
        for (size_t i = 0; i < g_out.size(); ++i) { le[2 * i] = (uint8_t)g_out[i]; le[2 * i + 1] = (uint8_t)(g_out[i] >> 8); }
        std::fwrite(le.data(), 1, le.size(), g_wav);
        g_wav_frames += g_out.size() / 2;
    }
    g_out.clear();
}

void sound_close() {
    std::fprintf(stderr, "  sound: %llu samples, %llu not silent; 68000 %s, pc %06X\n", (unsigned long long)g_samples,
                 (unsigned long long)g_peak_frames, g_on ? "on" : "off", m68k_get_reg(nullptr, M68K_REG_PC));
    host_audio_report();
    if (g_wav) {
        wav_header();
        std::fclose(g_wav);
        g_wav = nullptr;
    }
}

// ---- the SH-2's side: sound RAM at 0x05A00000, the registers at 0x05B00000 ------------------------
uint32_t sound_read(uint32_t a, int size) {
    if (a < 0x05B00000u) return mem_rd(g_sound_ram, a & 0x7FFFF, size);
    return scsp_read(a & 0xFFF, size);
}

void sound_write(uint32_t a, uint32_t v, int size) {
    if (a < 0x05B00000u) { mem_wr(g_sound_ram, a & 0x7FFFF, v, size); return; }
    scsp_write(a & 0xFFF, v, size);
}
