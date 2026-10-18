# The generated code's SH2_CYCLES against the instructions sh2emu steps through: every instruction that runs
# is counted once, on every path through labels, the emitter's gotos, delay slots, calls and jumps.
import os
import shutil
import subprocess

import pytest
from conftest import BASE, assemble

from saturnrecomp import sh2, sh2emu
from saturnrecomp.recomp import discover, emit

pytestmark = [pytest.mark.linux_only,
              pytest.mark.skipif(shutil.which("clang++") is None, reason="needs clang++ to build the generated code")]

INCLUDE = os.path.join(os.path.dirname(__file__), "..", "runtime", "include")
NOP, RTS = 0x0009, 0x000B
WRITE_CYCLES = 1                                 # kSh2WriteCycles; a read costs none


def stepped(code, entry, regs, t):
    """(cycles, r0) of the code run by sh2emu: each instruction's issue cycles, a taken branch's extra and
    each write the memory saw."""
    mem = sh2emu.Memory()
    mem.add(0x06000000, 0x100000, b"\0" * 0x4000 + code)
    cpu = sh2emu.CPU(mem)
    mem.log = []
    cpu.r[15], cpu.t, cpu.pr, cpu.pc = 0x060FFF00, t, sh2emu.RETURN_SENTINEL, entry
    for k, v in regs.items():
        cpu.r[k] = v
    cycles = 0
    while cpu.pc != sh2emu.RETURN_SENTINEL:
        pc = cpu.pc
        ins = cpu.fetch(pc)
        cycles += emit.cost(ins)[0] + (emit.cost(cpu.fetch(pc + 2))[0] if ins.delay else 0)
        cpu.step()
        if ins.op in ("bt", "bf") and cpu.pc != pc + 2:
            cycles += 2
        if ins.op in ("bt/s", "bf/s") and cpu.pc != pc + 4:
            cycles += 1
        assert cpu.steps < 100000
    return cycles + WRITE_CYCLES * sum(1 for e in mem.log if e[0] == "w"), cpu.r[0]


STUBS = """
uint8_t g_wram_h[0x100008]; uint8_t g_wram_l[0x100008]; uint64_t g_sh2_cycles[2];
uint32_t g_sh2_watch_lo, g_sh2_watch_len;
void sh2_watch_store(uint32_t, uint32_t, int) {}
uint32_t sh2_io_read(uint32_t, int) { std::abort(); }
void sh2_io_write(uint32_t, uint32_t, int) { std::abort(); }
void sh2_poll(SH2Context& c) { c.budget = 256; }
void sh2_sleep(SH2Context&, uint32_t) { std::abort(); }
void sh2_trapa(SH2Context&, uint32_t, uint32_t) { std::abort(); }
void sh2_hook(SH2Context&, uint32_t) {}
void sh2_bad_return(SH2Context&, uint32_t) { std::abort(); }
"""


def compiled(code, entry, regs, t, tmp):
    """(cycles, r0) of the code recompiled, built and run."""
    p = discover.Program(sh2.Image(code, BASE), [entry])
    names = sorted(p.funcs)
    src = ['#include "saturn/sh2.h"', "#include <cstdio>", "#include <cstdlib>", STUBS]
    src += ["void %s(SH2Context& c);" % emit.fname(a) for a in names]
    for a in names:
        src += emit.Body(p, p.funcs[a], set(p.funcs), comments=False).emit()
    src.append("void sh2_call(SH2Context& c, uint32_t a) { switch (a) {")
    src += ["case 0x%08Xu: %s(c); return;" % (a, emit.fname(a)) for a in names]
    src.append("} std::abort(); }")
    src.append("int main() { SH2Context c{}; c.budget = 256; c.r[15] = 0x060FFF00u; c.t = %d;" % t)
    src += ["c.r[%d] = %du;" % (k, v) for k, v in regs.items()]
    src.append('std::memcpy(g_wram_h + 0x4000, "%s", %d);' % ("".join("\\x%02x" % b for b in code), len(code)))
    src.append('c.pr = 0xFFFFFFF0u; %s(c); std::printf("%%llu %%u\\n", (unsigned long long)g_sh2_cycles[0], c.r[0]); }'
               % emit.fname(entry))
    cpp, exe = tmp / "p.cpp", tmp / "p"
    cpp.write_text("\n".join(src))
    subprocess.run(["clang++", "-std=c++17", "-O0", "-w", "-I", INCLUDE, str(cpp), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout.split()
    return int(out[0]), int(out[1])


PROGRAMS = {
    "a bf loop, and a write in rts's slot": ([0xE000, 0x7001, 0x8805, 0x8BFC, RTS, 0x2F12], BASE, {}),
    "a bt/s whose slot a later bt goes to": ([0xE103, 0x8D02, 0x71FF, 0x4115, 0x89FC, 0x7005, RTS, NOP], BASE, {}),
    "a bsr whose slot a later bra goes to": (
        [0x4F22, 0x7001, 0xB007, 0x7102, 0x4115, 0x8B02, 0x4F26, RTS, NOP, 0xAFF8, NOP, RTS, 0x7010],
        BASE, {1: 0xFFFFFFFE}),
    "a forward bf over a block": ([0x8800, 0x8B02, 0x7001, 0x7001, 0x7001, RTS, NOP], BASE, {}),
    "a bra over code, then a bf/s": ([0xA002, 0x7001, NOP, 0x7004, 0x8800, 0x8F01, 0x7008, 0x7010, RTS, NOP], BASE, {}),
    "stores with a displacement and an index": ([0x61F3, 0x1F00, 0x01F6, 0x8011, 0x7001, RTS, NOP], BASE, {}),
    "a jsr through the literal pool": (
        [0x4F22, 0xD103, 0x410B, 0x7001, 0x4F26, RTS, NOP, NOP, ("lit", BASE + 0x14), RTS, 0x7010], BASE, {}),
    "a jmp to its own code": ([0xD101, 0x412B, 0x7001, NOP, ("lit", BASE + 0x0C), 0x7002, RTS, NOP], BASE, {}),
    "code below the entry, reached by a bra": ([0x7001, RTS, NOP, 0x7002, 0xAFFA, 0x7004], BASE + 6, {}),
}


@pytest.mark.parametrize("name", sorted(PROGRAMS))
@pytest.mark.parametrize("t", [0, 1])
def test_the_generated_code_counts_the_cycles_of_what_runs(name, t, tmp_path):
    words, entry, regs = PROGRAMS[name]
    code = assemble(*words)
    assert compiled(code, entry, regs, t, tmp_path) == stepped(code, entry, regs, t)
