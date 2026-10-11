from conftest import BASE, assemble

from saturnrecomp import sh2
from saturnrecomp.recomp import discover, versions

NOP, RTS = 0x0009, 0x000B
BODY = [0xE000 | r << 8 | r for r in range(3, 15)] + [0xE00F, 0xE110]        # 14 movs


def side(*words):
    img = sh2.Image(assemble(*words), BASE)
    return img, discover.Program(img, [BASE])


def rows_by_a(a, b):
    rows, _ = versions.compare(a, b)
    return {x - BASE: (k, y and y - BASE) for k, x, y, *_ in rows if x}


def test_a_moved_function_is_the_same_and_an_edited_one_changed():
    a = side(
        0xB004, NOP,                             # 00 bsr 0C
        0xB00C, NOP,                             # 04 bsr 20
        RTS, NOP,                                # 08
        0xE101, 0xE202, 0xE303, 0xE404,          # 0C moves in B
        0xE505, 0xE606, 0xE707, 0xE808,
        RTS, NOP,
        0xB010, NOP,                             # 20 edited in B: calls 44, then BODY
        *BODY, RTS, NOP,
        0xE1AA, RTS, NOP,                        # 44 only A has it
    )
    b = side(
        0xB006, NOP,                             # 00 bsr 10
        0xB00E, NOP,                             # 04 bsr 24
        RTS, NOP,                                # 08
        NOP, NOP,                                # 0C
        0xE101, 0xE202, 0xE303, 0xE404,          # 10
        0xE505, 0xE606, 0xE707, 0xE808,
        RTS, NOP,
        0xE1F0, 0xE2F0,                          # 24 two movs where A's call was
        *BODY, RTS, NOP,
    )
    by = rows_by_a(a, b)
    assert by[0x00] == ("same", 0x00)
    assert by[0x0C] == ("same", 0x10)
    assert by[0x20] == ("changed", 0x24)
    assert by[0x44] == ("a-only", None)


def test_an_edited_caller_does_not_vote_for_the_functions_it_calls():
    a = side(
        0xB002, NOP,                             # 00 bsr 08, then a mov B lacks
        0xE155, RTS, NOP,
        0xE1AA, 0xE2AA, RTS, NOP,                # 08 only A has it
    )
    b = side(
        0xB002, NOP,                             # 00 bsr 08
        RTS, NOP,
        0xE1BB, 0xE2BB, 0xE3BB, RTS, NOP,        # 08 only B has it
    )
    by = rows_by_a(a, b)
    assert by[0x08] == ("a-only", None)
