from conftest import BASE, assemble

from saturnrecomp import sh2
from saturnrecomp.recomp import discover

NOP, RTS = 0x0009, 0x000B


def program(*words, seeds=()):
    return discover.Program(sh2.Image(assemble(*words), BASE), [BASE, *seeds])


def test_a_bsr_target_is_a_function():
    p = program(
        0xB004, NOP,                             # 00 bsr 0C
        RTS, NOP,                                # 04
        NOP, NOP,                                # 08 not reached
        RTS, NOP,                                # 0C
    )
    assert sorted(p.funcs) == [BASE, BASE + 0x0C]


def test_a_jsr_through_the_literal_pool_is_a_function():
    p = program(
        0xD102,                                  # 00 mov.l [0C],r1
        0x410B, NOP,                             # 02 jsr @r1
        RTS, NOP,                                # 06
        NOP,                                     # 0A
        ("lit", BASE + 0x10),                    # 0C
        RTS, NOP,                                # 10
    )
    assert BASE + 0x10 in p.funcs


def test_code_after_rts_is_not_part_of_the_function():
    p = program(RTS, NOP, 0xE105, RTS, NOP)
    assert BASE + 4 not in p.funcs[BASE].code
