import pytest
from conftest import BASE, assemble

from saturnrecomp import sh2
from saturnrecomp.recomp import discover, emit

NOP, RTS = 0x0009, 0x000B


def cost(word):
    return emit.cost(sh2.decode(word, BASE))


def test_an_instruction_costs_its_issue_cycles_and_its_reads_and_writes():
    assert cost(0x7001) == (1, 0, 0)            # add #1,r0
    assert cost(0x6012) == (1, 1, 0)            # mov.l @r1,r0
    assert cost(0x2012) == (1, 0, 1)            # mov.l r1,@r0
    assert cost(0x4F22) == (1, 0, 1)            # sts.l pr,@-r15
    assert cost(0xD102) == (1, 1, 0)            # mov.l from the literal pool
    assert cost(0x012F) == (2, 2, 0)            # mac.l @r2+,@r1+
    assert cost(0x411B) == (4, 1, 1)            # tas.b @r1
    assert cost(0xCC01) == (3, 1, 0)            # tst.b #1,@(r0,gbr)
    assert cost(0xCD01) == (3, 1, 1)            # and.b #1,@(r0,gbr)
    assert cost(0x4F27) == (3, 1, 0)            # ldc.l @r15+,vbr


def test_a_store_with_a_displacement_or_an_index_costs_a_write():
    for word in (0x1012,                        # mov.l r1,@(8,r0)
                 0x8011,                        # mov.b r0,@(1,r1)
                 0x8111,                        # mov.w r0,@(2,r1)
                 0x0014,                        # mov.b r1,@(r0,r0)
                 0x0016,                        # mov.l r1,@(r0,r0)
                 0xC001,                        # mov.b r0,@(1,gbr)
                 0xC201):                       # mov.l r0,@(4,gbr)
        assert cost(word) == (1, 0, 1), sh2.decode(word, BASE).text


def test_a_load_with_a_displacement_or_an_index_costs_a_read():
    for word in (0x5012,                        # mov.l @(8,r1),r0
                 0x001C,                        # mov.b @(r0,r1),r0
                 0xC601):                       # mov.l @(4,gbr),r0
        assert cost(word) == (1, 1, 0), sh2.decode(word, BASE).text


@pytest.mark.parametrize("word, cycles", [
    (0x002B, 4),                                # rte
    (0xC301, 8),                                # trapa #1
    (0x001B, 3),                                # sleep
    (0x0127, 2),                                # mul.l r2,r1
    (0x312D, 2),                                # dmuls.l r2,r1
    (0x3125, 2),                                # dmulu.l r2,r1
    (0x412F, 2),                                # mac.w @r2+,@r1+
    (0x4013, 2),                                # stc.l gbr,@-r0
])
def test_the_issue_cycles_of_the_slower_instructions(word, cycles):
    assert cost(word)[0] == cycles, sh2.decode(word, BASE).text


def test_a_jump_through_a_register_reads_no_memory():
    assert cost(0x410B) == (2, 0, 0)            # jsr @r1
    assert cost(0x412B) == (2, 0, 0)            # jmp @r1
    assert cost(RTS) == (2, 0, 0)


def lines(*words):
    p = discover.Program(sh2.Image(assemble(*words), BASE), [BASE])
    body = emit.Body(p, p.funcs[BASE], set(p.funcs), comments=False)
    return [l.strip() for l in body.emit()]


def test_each_straight_run_starts_with_its_cycles_and_a_taken_branch_adds_two():
    out = lines(
        0xE000,                                  # 00 mov #0,r0
        0x7001,                                  # 02 add #1,r0
        0x8805,                                  # 04 cmp/eq #5,r0
        0x8BFC,                                  # 06 bf 02
        RTS, 0x2012,                             # 08 rts ; mov.l r1,@r0
    )
    cycles = [l for l in out if l.startswith("SH2_CYCLES")]
    assert cycles == ["SH2_CYCLES(c, 1, 0, 0);",     # mov
                      "SH2_CYCLES(c, 3, 0, 0);",     # the loop: add, cmp/eq, bf
                      "SH2_CYCLES(c, 3, 0, 1);"]     # rts and its slot's write
    assert out[out.index("L_06004002:") + 1] == "SH2_CYCLES(c, 3, 0, 0);"
    assert any("SH2_CYCLES(c, 2, 0, 0); SH2_POLL(c); goto L_06004002;" in l for l in out)


def test_the_code_after_a_call_is_a_run_of_its_own():
    out = lines(
        0x7001,                                  # 00 add #1,r0
        0xB003, NOP,                             # 02 bsr 0C
        0x7001,                                  # 06 add #1,r0
        RTS, NOP,                                # 08
        RTS, NOP,                                # 0C
    )
    assert [l for l in out if l.startswith("SH2_CYCLES")] == [
        "SH2_CYCLES(c, 4, 0, 0);",                   # add, bsr, its slot
        "SH2_CYCLES(c, 4, 0, 0);"]                   # add, rts, its slot


def test_a_taken_delayed_branch_adds_one():
    out = lines(
        0x8D01, NOP,                             # 00 bt/s 06
        0x7001,                                  # 04 add #1,r0
        RTS, NOP,                                # 06
    )
    assert any("if (k) { SH2_CYCLES(c, 1, 0, 0);" in l for l in out)
