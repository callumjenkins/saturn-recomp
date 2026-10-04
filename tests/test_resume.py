from conftest import BASE, assemble

from saturnrecomp.analysis.resume import resume_points

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
