from conftest import BASE, assemble

from saturnrecomp import config
from saturnrecomp.analysis.resume import for_game, resume_points

NOP, RTS = 0x0009, 0x000B
G, F, Y, H = BASE, BASE + 0x08, BASE + 0x10, BASE + 0x14
CODE = assemble(
    0xB002, NOP, RTS, NOP,                       # G: bsr F
    0xB002, NOP, RTS, NOP,                       # F: bsr Y
    RTS, NOP,                                    # Y, the yield
    0x420B, NOP, RTS, NOP,                       # H: jsr @r2, a target nothing names
)


def points(tmp_path, unknown_yields=False):
    path = tmp_path / "prog.bin"
    path.write_bytes(CODE)
    return resume_points(str(path), BASE, [H], [Y], unknown_yields)


def test_a_call_to_a_yield_and_its_callers_resume_after_the_call(tmp_path):
    assert points(tmp_path) == [G + 4, F + 4]


def test_unknown_targets_count_as_yields_when_asked(tmp_path):
    assert points(tmp_path, unknown_yields=True) == [G + 4, F + 4, H + 4]


def test_a_function_that_jumps_to_a_yield_yields(tmp_path):
    path = tmp_path / "tail.bin"
    path.write_bytes(assemble(
        0xB002, NOP, RTS, NOP,                   # 00: bsr 08
        0xA000, NOP,                             # 08: bra 0C, its tail
        RTS, NOP,                                # 0C, the yield
    ))
    assert resume_points(str(path), BASE, [BASE + 0x0C], [BASE + 0x0C]) == [BASE + 4]


def test_the_function_holding_a_task_switch_yields(tmp_path):
    path = tmp_path / "prog.bin"
    path.write_bytes(CODE)
    assert resume_points(str(path), BASE, [H], [], inside=[Y]) == [G + 4, F + 4]


def test_a_program_with_a_stack_swapping_switch_gets_its_resume_points(tmp_path):
    (tmp_path / "prog.bin").write_bytes(CODE)
    (tmp_path / "game.toml").write_text(f"""
[[module]]
name = "MAIN"
file = "prog.bin"
base = {BASE:#x}
seeds = [{H:#x}]

[[task_switch]]
module = "MAIN"
save = {Y:#x}
save_key = "r1"
resume_sp = 84
switch = {Y:#x}
switch_key = "r0"
""")
    assert for_game(config.load(str(tmp_path / "game.toml"))) == {"MAIN": [G + 4, F + 4]}
