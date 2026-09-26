// saturnkit runtime — what the sound side shares: the SCSP (scsp.cpp), the
// 68000 and the timing that drives both (sound.cpp), and where the samples
// go (host.cpp's audio stream, a WAV file).
#pragma once
#include <cstdint>

// ---- the SCSP (scsp.cpp) ----------------------------------------------------------------
extern uint8_t g_sound_ram[0x80000];                  // 512 KB, big-endian, shared by the 68000 and the SH-2
void scsp_reset();
uint32_t scsp_read(uint32_t off, int size);           // registers, off 0x000-0xFFF
void scsp_write(uint32_t off, uint32_t v, int size);
void scsp_sample(int16_t exts_l, int16_t exts_r, int16_t out[2]);   // one sample at 44 100 Hz
int  scsp_cpu_level();                                // the 68000's interrupt level (0: none)
uint32_t scsp_active_slots();                         // a mask, for the log

// ---- the 68000 and the pace (sound.cpp) ----------------------------------------------------
void sound_irq_changed();                             // the SCSP's interrupt to the 68000 moved
void sound_main_irq();                                // the SCSP asks the SCU for a sound request

// ---- the host (host.cpp) ------------------------------------------------------------------
bool host_audio_open();                               // with a window; false: no device
void host_audio_push(const int16_t* lr, int frames);  // interleaved stereo, 44 100 Hz
void host_audio_report();                             // how the stream fared, to stderr
