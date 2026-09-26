# Musashi 4.60

The 68000 of saturnkit's sound side (`../../sound.cpp`): Karl Stenerud's
Musashi, MIT licence (the notice is at the top of `m68kcpu.c` and in
`readme.txt`), with MAME's repackaging of John R. Hauser's SoftFloat 2b in
`softfloat/` (its own permissive notice, `softfloat/README.txt` and the
source headers), which the FPU code compiled into `m68kcpu.c` needs.

Taken as released, with `m68kops.c`/`m68kops.h` as `m68kmake` generates
them from 4.60's `m68k_in.c`. One change, in `m68kconf.h`: the 68010,
68EC020, 68020, 68030 and 68040 and the PMMU are switched off
(`M68K_EMULATE_* M68K_OPT_OFF`); the Saturn's sound CPU is a plain 68000
(68EC000). The memory callbacks (`m68k_read_memory_*`,
`m68k_write_memory_*`, `m68k_read_disassembler_*`) are `sound.cpp`'s.
