import pytest

from saturnrecomp import sh2

PC = 0x06004000


@pytest.mark.parametrize("fmt", sh2.formats())
def test_every_format_decodes_back_to_itself(fmt):
    word = sh2.encode(fmt, n=3, m=5, low=1)
    assert sh2.decode(word, PC).fmt == fmt


@pytest.mark.parametrize("word, text", [
    (0x0009, "nop"),
    (0x000B, "rts"),
    (0xE105, "mov #5,r1"),
    (0xE1FF, "mov #-1,r1"),
    (0x410B, "jsr @r1"),
    (0xB002, "bsr 0x06004008"),                  # pc + 4 + 2 * 2
    (0xD101, "mov.l 0x06004008,r1"),             # (pc & ~3) + 4 + 1 * 4
])
def test_disassembly(word, text):
    assert sh2.decode(word, PC).text == text


def test_branch_marks_its_delay_slot():
    assert sh2.decode(0xB002, PC).delay
    assert not sh2.decode(0x0009, PC).delay
