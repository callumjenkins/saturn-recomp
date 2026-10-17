from conftest import sound_program

from saturnrecomp import m68k

RTS, NOP = 0x4E75, 0x4E71


def program(lib, *words, **kw):
    return m68k.Program(m68k.Image(sound_program(*words, **kw), lib))


def test_a_descent_from_the_vectors_follows_calls_loops_and_branches_to_each_return(m68k_lib):
    p = program(m68k_lib,
                0x6104,              # 400 bsr.s 406
                0x51C8, 0xFFFE,      # 402 dbra d0,402
                0x6702,              # 406 beq.s 40A
                RTS,                 # 408
                RTS,                 # 40A
                0x7001)              # 40C moveq #1,d0: nothing reaches it
    assert sorted(p.code) == [0x400, 0x402, 0x406, 0x408, 0x40A]
    assert p.data == set(range(0x400)) and p.gaps() == [(0x40C, 2)]
    assert not p.bad and not p.unresolved


def test_a_vector_that_is_odd_inside_the_table_or_past_the_image_is_no_entry(m68k_lib):
    p = program(m68k_lib, RTS, RTS, vectors=[(8, 0x401), (0xC, 0x200), (0x10, 0x10000), (0x14, 0x402)])
    assert p.entries == [0x400, 0x402]


def test_a_branch_table_bounded_by_andi_has_an_entry_for_each_value_of_the_mask(m68k_lib):
    p = program(m68k_lib,
                0x0242, 0x0003,      # 400 andi.w #3,d2
                0xD442,              # 404 add.w d2,d2
                0x4EFB, 0x2002,      # 406 jmp (2,pc,d2.w): the table at 40A
                RTS, RTS, RTS, RTS,  # 40A-410
                RTS)                 # 412, past the mask
    assert p.tables == [(0x406, "branches", 0x40A, 4)]
    assert 0x410 in p.code and 0x412 not in p.code


def test_a_mask_on_an_index_already_scaled_takes_only_its_multiples(m68k_lib):
    p = program(m68k_lib,
                0x0240, 0x0004,      # 400 andi.w #4,d0
                0x4EFB, 0x0002,      # 404 jmp (2,pc,d0.w): the table at 408
                0x6000, 0x000A,      # 408 bra.w 414
                0x6000, 0x0008,      # 40C bra.w 416
                0x7001, 0x7001,      # 410: an entry only if the mask were read as a count of entries
                RTS, RTS)            # 414, 416
    assert p.tables == [(0x404, "branches", 0x408, 2)]
    assert 0x410 not in p.code and {0x414, 0x416} <= p.code.keys()


def test_a_cmpi_and_bcc_bound_counts_the_values_below_the_compared_one(m68k_lib):
    p = program(m68k_lib,
                0x0C47, 0x0002,      # 400 cmpi.w #2,d7
                0x6408,              # 404 bcc.s 40E
                0xDE47, 0xDE47,      # 406 add.w d7,d7 twice
                0x4EBB, 0x7004,      # 40A jsr (4,pc,d7.w): the table at 410
                RTS,                 # 40E
                NOP, RTS,            # 410, entry 0
                RTS, NOP,            # 414, entry 1
                RTS)                 # 418, entry 2 only were the bound 3
    assert p.tables == [(0x40A, "branches", 0x410, 2)]
    assert p.gaps() == [(0x416, 4)]


def test_an_index_copied_from_a_bounded_register_keeps_its_bound(m68k_lib):
    p = program(m68k_lib,
                0x0243, 0x0001,      # 400 andi.w #1,d3
                0x3003,              # 404 move.w d3,d0
                0xD040,              # 406 add.w d0,d0
                0x4EFB, 0x0002,      # 408 jmp (2,pc,d0.w): the table at 40C
                RTS, RTS, RTS)       # 40C, 40E, then 410 past the bound
    assert p.tables == [(0x408, "branches", 0x40C, 2)]


def test_a_table_reached_through_a_jump_keeps_the_bound_found_before_the_jump(m68k_lib):
    p = program(m68k_lib,
                0x0242, 0x0001,      # 400 andi.w #1,d2
                0xD442,              # 404 add.w d2,d2
                0x4EFA, 0x0002,      # 406 jmp (2,pc): 40A
                0x4EFB, 0x2002,      # 40A jmp (2,pc,d2.w): the table at 40E
                RTS, RTS, RTS)       # 40E, 410, then 412 past the bound
    assert p.tables == [(0x40A, "branches", 0x40E, 2)]


def test_an_offset_table_with_no_bound_ends_at_its_lowest_target(m68k_lib):
    p = program(m68k_lib,
                0x3E02,              # 400 move.w d2,d7, unbounded
                0xDE47,              # 402 add.w d7,d7
                0x363B, 0x7006,      # 404 move.w (6,pc,d7.w),d3: the offsets at 40C
                0x4EFB, 0x3002,      # 408 jmp (2,pc,d3.w): from 40C
                0x0004, 0x0006,      # 40C offsets to 410 and 412
                RTS, RTS)            # 410, 412
    assert p.tables == [(0x408, "offsets", 0x40C, 2)]
    assert {0x410, 0x412} <= p.code.keys() and set(range(0x40C, 0x410)) <= p.data and p.gaps() == []


def test_an_unbounded_table_ends_before_an_entry_whose_target_lands_inside_an_instruction(m68k_lib):
    p = program(m68k_lib,
                0x3E02, 0xDE47,      # 400 move.w d2,d7; add.w d7,d7
                0x327B, 0x7004,      # 404 movea.w (4,pc,d7.w),a1: the addresses at 40A
                0x4ED1,              # 408 jmp (a1)
                0x0414, 0x0418,      # 40A 414, then 418, inside the move.l at 416
                0x0000, 0x0000,      # 40E
                0x4E71, 0x7001,      # 412 nop; moveq #1,d0
                0x203C, 0x0000, RTS,  # 416 move.l #$4e75,d0
                RTS)                 # 41C
    assert p.tables == [(0x408, "words", 0x40A, 1)]
    assert p.bad == {} and sorted(p.code) == [0x400, 0x402, 0x404, 0x408, 0x414, 0x416, 0x41C]
    assert 0x40C not in p.data


def test_an_address_table_read_through_lea_and_movea_l(m68k_lib):
    p = program(m68k_lib,
                0x0240, 0x0001,      # 400 andi.w #1,d0
                0xD040, 0xD040,      # 404 add.w d0,d0 twice
                0x43FA, 0x000A,      # 408 lea (10,pc),a1: 414
                0x2271, 0x0000,      # 40C movea.l (0,a1,d0.w),a1
                0x4ED1,              # 410 jmp (a1)
                NOP,                 # 412
                ("lit", 0x41C), ("lit", 0x41E),
                RTS, RTS)            # 41C, 41E
    assert p.tables == [(0x410, "longs", 0x414, 2)]
    assert {0x41C, 0x41E} <= p.code.keys() and set(range(0x414, 0x41C)) <= p.data and p.gaps() == [(0x412, 2)]


def test_a_word_address_table_read_by_movea_w(m68k_lib):
    p = program(m68k_lib,
                0x0240, 0x0001,      # 400 andi.w #1,d0
                0xD040,              # 404 add.w d0,d0
                0x327B, 0x0004,      # 406 movea.w (4,pc,d0.w),a1: 40C
                0x4ED1,              # 40A jmp (a1)
                0x0410, 0x0412,      # 40C
                RTS, RTS)            # 410, 412
    assert p.tables == [(0x40A, "words", 0x40C, 2)]
    assert {0x410, 0x412} <= p.code.keys() and set(range(0x40C, 0x410)) <= p.data


def test_a_jump_through_a_register_no_table_loads_is_unresolved(m68k_lib):
    p = program(m68k_lib, 0x4ED0)             # jmp (a0)
    assert p.unresolved == {0x400} and p.tables == []


def test_a_word_that_does_not_decode_ends_its_path_as_bad(m68k_lib):
    p = program(m68k_lib, 0x7001, 0xFFFF, RTS)
    assert sorted(p.code) == [0x400] and p.bad == {0x402: "does not decode"}


def test_a_branch_into_the_middle_of_an_instruction_is_bad(m68k_lib):
    p = program(m68k_lib,
                0x6704,              # 400 beq.s 406
                0x203C, 0x0000, RTS,  # 402 move.l #$4e75,d0
                RTS)                 # 408
    assert sorted(p.code) == [0x400, 0x402, 0x408] and p.bad == {0x406: "overlaps 402"}


def test_an_indexed_operand_has_the_68000s_length_whatever_bit_8_says(m68k_lib):
    p = program(m68k_lib, 0x2030, 0x0120, RTS)   # move.l ($20,a0,d0.w),d0, read by a 68020 as full format
    assert p.code == {0x400: 4, 0x404: 2}


def test_a_gap_reports_its_content_returns_pointers_and_what_decodes(m68k_lib):
    p = program(m68k_lib,
                0x41FA, 0x0006,      # 400 lea (6,pc),a0: 408
                RTS,                 # 404
                0x7001, 0x7002, RTS,  # 406, nothing reaches it
                0x7003)              # 40C
    assert m68k.evidence(p) == [(0x406, 8, "other", 1, 1, 3)]


def test_an_index_written_after_its_bound_has_no_bound(m68k_lib):
    p = program(m68k_lib,
                0x0240, 0x0001,      # 400 andi.w #1,d0
                0xD041,              # 404 add.w d1,d0: any value again
                0xD040,              # 406 add.w d0,d0
                0x4EFB, 0x0002,      # 408 jmp (2,pc,d0.w): the table at 40C
                RTS, RTS, RTS)       # 40C-410, read while each is a branch or return
    assert p.tables == [(0x408, "branches", 0x40C, 3)]


def test_the_taken_side_of_a_bounding_bcc_is_unbounded(m68k_lib):
    p = program(m68k_lib,
                0x0C47, 0x0002,      # 400 cmpi.w #2,d7
                0x640C,              # 404 bcc.s 412, where d7 is 2 or more
                0xDE47,              # 406 add.w d7,d7
                0x4EFB, 0x7002,      # 408 jmp (2,pc,d7.w): the table at 40C
                RTS, RTS, RTS,       # 40C, 40E, then 410 past the bound
                0xDE47,              # 412 add.w d7,d7
                0x4EFB, 0x7002,      # 414 jmp (2,pc,d7.w): the table at 418
                RTS, RTS, RTS, RTS)  # 418-41E
    assert sorted(p.tables) == [(0x408, "branches", 0x40C, 2), (0x414, "branches", 0x418, 4)]


def test_a_cmpi_and_bhi_bound_counts_the_compared_value_too(m68k_lib):
    p = program(m68k_lib,
                0x0C47, 0x0001,      # 400 cmpi.w #1,d7
                0x6206,              # 404 bhi.s 40C
                0xDE47,              # 406 add.w d7,d7
                0x4EFB, 0x7002,      # 408 jmp (2,pc,d7.w): the table at 40C
                RTS, RTS, RTS)       # 40C, 40E, then 410 past the bound
    assert p.tables == [(0x408, "branches", 0x40C, 2)]


def test_a_cmpi_followed_by_another_branch_is_no_bound(m68k_lib):
    p = program(m68k_lib,
                0x0C47, 0x0001,      # 400 cmpi.w #1,d7
                0x6706,              # 404 beq.s 40C
                0xDE47,              # 406 add.w d7,d7
                0x4EFB, 0x7002,      # 408 jmp (2,pc,d7.w): the table at 40C
                RTS, RTS, RTS)       # 40C-410, read while each is a branch or return
    assert p.tables == [(0x408, "branches", 0x40C, 3)]


def test_a_shift_scales_the_bounded_index_and_andi_b_bounds_it(m68k_lib):
    p = program(m68k_lib,
                0x0200, 0x0001,      # 400 andi.b #1,d0
                0xE548,              # 404 lsl.w #2,d0
                0x4EFB, 0x0002,      # 406 jmp (2,pc,d0.w): the table at 40A
                RTS, NOP, RTS, NOP,  # 40A, 40E: entries 4 bytes apart
                RTS)                 # 412 past the bound
    assert p.tables == [(0x406, "branches", 0x40A, 2)]
    assert {0x40A, 0x40E} <= p.code.keys() and 0x412 not in p.code


def test_an_unbounded_branch_table_reads_branches_at_the_index_scale(m68k_lib):
    p = program(m68k_lib,
                0xD040, 0xD040,      # 400 add.w d0,d0 twice
                0x4EFB, 0x0002,      # 404 jmp (2,pc,d0.w): the table at 408
                0x4EFA, 0x000A,      # 408 jmp (10,pc): 414
                0x6000, 0x0008,      # 40C bra.w 416
                0x7001, 0x7001,      # 410 not a branch: the table ends
                RTS, RTS)            # 414, 416
    assert p.tables == [(0x404, "branches", 0x408, 2)]
    assert 0x410 not in p.code and {0x414, 0x416} <= p.code.keys()


def test_an_unbounded_table_stops_at_its_lowest_target_above_it(m68k_lib):
    p = program(m68k_lib,
                0xDE47,              # 400 add.w d7,d7
                0x327B, 0x7004,      # 402 movea.w (4,pc,d7.w),a1: the addresses at 408
                0x4ED1,              # 406 jmp (a1)
                0x0400,              # 408 400, below the table: the stop stays put
                0x040E,              # 40A 40E
                0x0410,              # 40C 410
                RTS, RTS)            # 40E: the table stops here; 410
    assert p.tables == [(0x406, "words", 0x408, 3)]


def test_an_unbounded_table_ends_before_a_target_that_does_not_decode(m68k_lib):
    p = program(m68k_lib,
                0xDE47,              # 400 add.w d7,d7
                0x327B, 0x7004,      # 402 movea.w (4,pc,d7.w),a1: the addresses at 408
                0x4ED1,              # 406 jmp (a1)
                0x040C, 0x040E,      # 408
                RTS, 0xFFFF)         # 40C, 40E
    assert p.tables == [(0x406, "words", 0x408, 1)] and p.bad == {}


def test_a_bounded_table_keeps_a_target_that_does_not_decode(m68k_lib):
    p = program(m68k_lib,
                0x0240, 0x0001,      # 400 andi.w #1,d0
                0xD040,              # 404 add.w d0,d0
                0x4EFB, 0x0002,      # 406 jmp (2,pc,d0.w): the table at 40A
                RTS, 0xFFFF)         # 40A, 40C
    assert p.tables == [(0x406, "branches", 0x40A, 2)] and p.bad == {0x40C: "does not decode"}


def test_a_long_address_table_read_by_movea_l(m68k_lib):
    p = program(m68k_lib,
                0x0240, 0x0001,      # 400 andi.w #1,d0
                0xD040, 0xD040,      # 404 add.w d0,d0 twice
                0x227B, 0x0004,      # 408 movea.l (4,pc,d0.w),a1: 40E
                0x4ED1,              # 40C jmp (a1)
                ("lit", 0x416), ("lit", 0x418),
                RTS, RTS)            # 416, 418
    assert p.tables == [(0x40C, "longs", 0x40E, 2)] and {0x416, 0x418} <= p.code.keys()


def test_a_word_loaded_into_another_register_makes_no_offset_table(m68k_lib):
    p = program(m68k_lib,
                0x0242, 0x0000,      # 400 andi.w #0,d2
                0x323B, 0x7006,      # 404 move.w (6,pc,d7.w),d1, which the jump does not use
                0x4EFB, 0x2002,      # 408 jmp (2,pc,d2.w): the table at 40C
                RTS)                 # 40C
    assert p.tables == [(0x408, "branches", 0x40C, 1)]


def test_a_table_base_lea_loads_into_another_register_leaves_the_jump_unresolved(m68k_lib):
    p = program(m68k_lib,
                0x45FA, 0x0008,      # 400 lea (8,pc),a2
                0x2271, 0x0000,      # 404 movea.l (0,a1,d0.w),a1
                0x4ED1)              # 408 jmp (a1)
    assert p.unresolved == {0x408} and p.tables == []


def test_an_absolute_jump_and_call_are_followed(m68k_lib):
    p = program(m68k_lib,
                0x4EB9, 0x0000, 0x040C,  # 400 jsr $40c.l
                0x4EF8, 0x040E,      # 406 jmp $40e.w
                NOP,                 # 40A
                RTS, RTS)            # 40C, 40E
    assert sorted(p.code) == [0x400, 0x406, 0x40C, 0x40E]


def test_a_dbra_falls_through_to_the_next_instruction(m68k_lib):
    p = program(m68k_lib, 0x51C8, 0xFFFE, RTS)     # dbra d0,400; rts
    assert sorted(p.code) == [0x400, 0x404]


def test_a_gaps_content_is_zero_fill_text_or_other():
    assert [m68k.content(c) for c in (b"\0\0\0\0", b"\x4e\x71\xff\xff", b"SOUND DRV", b"\x12\x34")] == [
        "zero", "fill", "text", "other"]


def test_a_gap_that_never_returns_decodes_nothing_and_one_past_fill_counts_from_its_code(m68k_lib):
    loops = program(m68k_lib, RTS, 0x7001, 0x60FC)              # 402 moveq; bra.s 402
    assert m68k.evidence(loops) == [(0x402, 4, "other", 0, 0, 0)]
    filled = program(m68k_lib, RTS, NOP, NOP, 0x7001, RTS)      # 402 nop nop, then moveq; rts
    assert m68k.evidence(filled) == [(0x402, 8, "other", 1, 0, 2)]
