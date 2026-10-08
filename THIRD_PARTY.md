# Third-party code in saturn-recomp

saturn-recomp is MIT-licensed (`LICENSE`). Some parts of it come from, or
follow, other people's work under their own permissive licences. One optional
file follows Mednafen and is under the GPL; a build leaves it out unless asked
(the last section).

## Musashi (the 68000) — MIT

`runtime/third_party/musashi/`: Musashi 4.60 by Karl Stenerud, taken as
released with one configuration change (`README.md` there). Its licence
is at the top of `m68kcpu.c` and in `readme.txt`:

> Copyright © 1998-2001 Karl Stenerud
>
> Permission is hereby granted, free of charge, to any person obtaining a
> copy of this software and associated documentation files (the
> "Software"), to deal in the Software without restriction, including
> without limitation the rights to use, copy, modify, merge, publish,
> distribute, sublicense, and/or sell copies of the Software, and to permit
> persons to whom the Software is furnished to do so, subject to the
> following conditions:
>
> The above copyright notice and this permission notice shall be included
> in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
> OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
> MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
> NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
> DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
> OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
> USE OR OTHER DEALINGS IN THE SOFTWARE.

## Dear ImGui (the launcher's menu) — MIT

`runtime/third_party/imgui/`: Dear ImGui 1.92.9b by Omar Cornut and its contributors, the core and
the SDL3 and SDL_Renderer3 backends, unchanged (`VERSION` there). Only the SDL host builds it. Its
licence is `LICENSE.txt` there: the MIT licence above, copyright © 2014-2026 Omar Cornut.

## SoftFloat (inside Musashi) — SoftFloat 2b's own terms

`runtime/third_party/musashi/softfloat/`: John R. Hauser's SoftFloat
Release 2b, as MAME repackaged it and Musashi ships it (Musashi's FPU code
needs it). Its terms are the four paragraphs at the top of `softfloat.c`
and `softfloat.h`, kept there unchanged: derivative works are acceptable,
even commercial ones, so long as the source says it is derivative and keeps
those paragraphs.

## minicoro (the game's tasks) — MIT No Attribution, with MIT parts

`runtime/third_party/minicoro/minicoro.h`: minicoro 0.2.0 by Eduardo
Bart, as of commit 02dad0f8 on GitHub, unchanged. The file is under the
choice of public domain or MIT No Attribution, both at its end. Its assembly
context switches are taken from Mike Pall's LuaCoco, under the MIT licence
quoted beside them in the file.

## MAME's SCSP (the envelope, the LFOs, FM, the DSP) — BSD-3-Clause

`runtime/scsp.cpp` is saturn-recomp's own code, but its envelope times and key
scaling, its LFO tables and scales, its FM scaling and its DSP step (with
the ring buffer's floating-point format) follow MAME's `scsp.cpp` and
`scspdsp.cpp` closely enough to count as derived from them. Those files
carry `license:BSD-3-Clause`, `copyright-holders:ElSemi, R. Belmont`
(`thanks-to:kingshriek`):

> Copyright (c) ElSemi, R. Belmont
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are
> met:
>
> 1. Redistributions of source code must retain the above copyright notice,
>    this list of conditions and the following disclaimer.
>
> 2. Redistributions in binary form must reproduce the above copyright
>    notice, this list of conditions and the following disclaimer in the
>    documentation and/or other materials provided with the distribution.
>
> 3. Neither the name of the copyright holder nor the names of its
>    contributors may be used to endorse or promote products derived from
>    this software without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
> IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
> THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
> PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
> CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
> EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
> PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
> PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
> LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
> NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
> SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## Read, not copied

Mednafen (GPL-2.0) and Beetle Saturn were read for facts about the
hardware (the SCSP timers' behaviour, the pan and level scales) and are
used as the oracle the ports are compared with; no code of theirs is in
saturn-recomp.

## Mednafen's VDP1 rasterising (optional) — GPL v2 or later

`runtime/src/video/vdp1_raster_gpl.inc` draws VDP1's sprites, polygons and
lines by the rules of Mednafen 1.32.1's VDP1 (`src/ss/vdp1.cpp`,
`vdp1_common.h`, `vdp1_sprite.cpp`, `vdp1_poly.cpp`, `vdp1_line.cpp`;
Copyright (C) 2015-2020 Mednafen Team), and is under the same licence: the
GNU General Public License, version 2 or (at your option) any later version.

It is compiled only when the runtime is built with `-DSATURN_VDP1_GPL=ON`
(a game asks for it with `cmake = ["SATURN_VDP1_GPL=ON"]` in its
`game.toml`). Such a build is a GPL work as a whole: whoever distributes it
must offer its complete source under the GPL. Without the option the file is
not compiled, `vdp1_raster.inc` draws the shapes instead, and the build is
under the MIT licence and the permissive terms above.

