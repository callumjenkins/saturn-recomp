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


def test_a_prologue_after_a_scheduled_instruction_opens_a_function():
    p = program(
        RTS, NOP,                                # 00
        0xE300,                                  # 04 mov #0,r3, ahead of the frame
        0x4F22,                                  # 06 sts.l pr,@-r15
        0x4F26,                                  # 08 lds.l @r15+,pr
        RTS, NOP,                                # 0A
    )
    assert BASE + 4 in p.funcs


def test_an_unbounded_table_of_code_addresses_is_a_switch():
    p = program(
        0xD302,                                  # 00 mov.l [0C],r3
        0x033E,                                  # 02 mov.l @(r0,r3),r3
        0x70FC,                                  # 04 add #-4,r0
        0x432B, NOP,                             # 06 jmp @r3
        NOP,                                     # 0A
        ("lit", BASE + 0x10),                    # 0C the table
        ("lit", BASE + 0x18), ("lit", BASE + 0x1A),
        0xE101,                                  # 18 mov #1,r1
        NOP,                                     # 1A
        RTS, NOP,                                # 1C
    )
    assert BASE + 0x06 in p.switches
    assert {BASE + 0x18, BASE + 0x1A} <= p.funcs[BASE].code
