/* saturn-recomp runtime — the 68000's registers and flags for a dump (state.cpp).
   Musashi keeps them at the front of its CPU struct, ahead of its cycle tables and the host's
   callbacks, which stay as this run set them. */
#include <stddef.h>
#include <string.h>
#include "m68kcpu.h"

size_t m68k_state_size(void) { return offsetof(m68ki_cpu_core, cyc_instruction); }

void m68k_state_get(void* out) { memcpy(out, &m68ki_cpu, m68k_state_size()); }

void m68k_state_set(const void* in) { memcpy(&m68ki_cpu, in, m68k_state_size()); }
