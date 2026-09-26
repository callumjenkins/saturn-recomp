// saturnkit runtime — the Saturn under the recompiled code: the machine
// (time, the two CPUs, program starts), the HLE BIOS, and the devices at
// their registers. sh2.h is the recompiled code's view; this header is the
// devices' view of each other.
//
//   machine.cpp   the run loop, time, interrupts to a CPU, the slave as a
//                 coroutine, program starts (the host stack unwound)
//   bios.cpp      the boot (IP.BIN, the 1st read file) and the BIOS services
//   mmio.cpp      the address map outside the work RAMs, and the access log
//   scu.cpp       interrupt controller, timers, DMA
//   smpc.cpp      commands, INTBACK, the pads
//   cdblock.cpp   the CD block at its registers, over cdrom.cpp's disc
//   onchip.cpp    the SH7604's own registers, per CPU (DIVU, FRT, DMAC...)
//   video.cpp     the raster timing, the video and sound chips on the bus, the sound driver
//   vdp1.cpp      VDP1: the command table drawn in software, the framebuffers
//   vdp2.cpp      VDP2: the picture composed from its layers and VDP1's
//   host.cpp      the window, the pad, the pace, PNG files
#pragma once
#include "sh2.h"
#include <cstdint>
#include <string>

// ---- the machine (machine.cpp) -------------------------------------------------------
struct SaturnConfig {
    std::string cue;                // the disc
    std::string out = ".";          // logs and the backup memory go here
    bool realtime = false;          // host clock; otherwise virtual time from safe points
    uint64_t stop_vblanks = 0;      // end the run after this many VBlanks (0: never)
    int stop_starts = 0;            // ... or after this many program starts (0: never)
    bool trace = false;             // events to stderr
    std::string peek;               // ADDR[:WORDS],... : memory printed when the run ends
    uint32_t watch_lo = 1, watch_hi = 0;   // memory-area addresses logged one by one
    std::string input;              // the pad: "VBLANK:BUTTON+BUTTON,..."
    std::string dump;               // N,... : the video memories to out/dump-N.bin at VBlank-IN N
    std::string shots;              // N,... : the picture to out/shot-N.png at VBlank-IN N
    bool headless = false;          // no window
    bool fullscreen = false;
    int scale = 3;                  // the window: 320x240 times this
};
extern SaturnConfig g_cfg;
extern SH2Context g_master, g_slave;
extern SH2Context* g_cpu;           // the CPU running now

int  saturn_main(const SaturnConfig& cfg);
uint64_t sat_now();                 // ns since power-on (virtual or host)
uint64_t sat_vblanks();
void sat_trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void sat_note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));   // always printed
[[noreturn]] void sat_fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void sat_stop(const char* why);                  // ends the run cleanly
void sat_interrupt(SH2Context& c, uint32_t vec, uint32_t level);   // take it now (a safe point)
void slave_on();                    // SMPC SSHON: the slave boots
void slave_off();                   // SSHOFF: held in reset
void slave_idle_check(uint32_t ftcsr);   // the slave read its FTCSR: yield if nothing is there
void slave_kick();                  // SINIT: the slave's input capture, and the CPU to it
void master_poll_devices();

// ---- the BIOS (bios.cpp) -----------------------------------------------------------
void bios_boot();                   // the state the BIOS leaves, the 1st read file loaded
uint32_t bios_first_read();         // where it was loaded (the entry)
bool bios_pal();                    // IP.BIN's areas: Europe alone, a PAL disc
bool bios_call(SH2Context& c, uint32_t addr);        // a BIOS ROM address: true if handled
bool bios_is_dispatcher(uint32_t vec, uint32_t target);
void bios_dispatch(SH2Context& c, uint32_t vec);     // the handler SYS_SETUINT installed
void bios_save();                   // the backup memory to its file

// ---- the address map (mmio.cpp) ---------------------------------------------------
void mmio_log_write(const std::string& path);        // every register touched, with counts
void mmio_log_call(uint32_t target, uint32_t from);  // a call to a non-entry
void mmio_watch(uint32_t lo, uint32_t hi);            // log each address of a memory area in the range
struct MemArea { uint8_t* p; uint32_t size; };        // byte arrays, big-endian guest order
uint32_t mem_rd(const uint8_t* p, uint32_t o, int size);
void     mem_wr(uint8_t* p, uint32_t o, uint32_t v, int size);

// ---- SCU (scu.cpp) ----------------------------------------------------------------
enum ScuIrq {
    IRQ_VBLANK_IN = 0, IRQ_VBLANK_OUT, IRQ_HBLANK_IN, IRQ_TIMER0, IRQ_TIMER1, IRQ_DSP_END,
    IRQ_SOUND_REQ, IRQ_SMPC, IRQ_PAD, IRQ_DMA2, IRQ_DMA1, IRQ_DMA0, IRQ_DMA_ILLEGAL,
    IRQ_SPRITE_END,
};
void scu_raise(int irq);
void scu_set_mask(uint32_t ims);
uint32_t scu_mask();
bool scu_deliver(SH2Context& c);    // take pending, unmasked interrupts above c's mask; any taken?
void scu_frame_event(int what);     // DMA start factors: 0 VBlank-IN, 1 VBlank-OUT, 6 sprite end
void scu_line(int line);            // timer 0 compare, per raster line reached
uint32_t scu_read(uint32_t off, int size);
void scu_write(uint32_t off, uint32_t v, int size);

// ---- SMPC (smpc.cpp) ----------------------------------------------------------------
uint32_t smpc_read(uint32_t off);
void smpc_write(uint32_t off, uint32_t v);
void smpc_tick();
void smpc_input_script(const std::string& spec);   // "VBLANK:BUTTON+BUTTON,..."

// ---- CD block (cdblock.cpp, cdrom.cpp) ------------------------------------------------
bool cdrom_open(const std::string& cue);
bool cdrom_read(uint32_t fad, uint8_t* raw2352);      // false outside the disc
bool cdrom_is_audio(uint32_t fad);
struct CdTrack { uint32_t fad, ctrladr; };
int  cdrom_tracks(CdTrack* out, int max);             // number of tracks
uint32_t cdrom_leadout();
bool cdrom_find(const char* path, uint32_t* fad, uint32_t* size);   // ISO 9660, "DIR/FILE.EXT"
std::string cdrom_first_file();                       // the 1st file of the root: the 1st read file
void cd_init();
void cd_tick();
uint32_t cd_read(uint32_t off, int size);
void cd_write(uint32_t off, uint32_t v, int size);

// ---- SH7604 on-chip (onchip.cpp) ------------------------------------------------------
uint32_t onchip_read(SH2Context& c, uint32_t a, int size);
void onchip_write(SH2Context& c, uint32_t a, uint32_t v, int size);
void onchip_input_capture(int cpu);   // SINIT/MINIT: the FRT's input-capture flag
void onchip_reset(int cpu);

// ---- video and sound (video.cpp; vdp1.cpp, vdp2.cpp, host.cpp through video.h) ------------------
void video_init();
void video_tick(uint64_t now);        // raster timing: VBlank in/out, lines, VDP1 frames
uint32_t video_read(uint32_t a, int size);    // VDP1, VDP2, SCSP (canonical addresses)
void video_write(uint32_t a, uint32_t v, int size);
bool video_owns(uint32_t a);
void sound_power(bool on);            // SMPC SNDON/SNDOFF: the 68000 runs or is held
void sound_tick();                    // the sound driver's side of its command handshake
uint64_t video_frame_changes();
uint64_t video_draws();
