from saturnrecomp.native import merge, parse

TABLE = """
CU: ./p_main_000.cpp:
File name                            Line number    Starting address    View    Stmt
p_main_000.cpp                                 5            0x1000               x
saturn.h                                      34            0x1004               x
p_main_000.cpp                                 6            0x1010               x
p_main_000.cpp                                 0            0x1018
p_main_000.cpp                                 7            0x1020               x
p_main_000.cpp                                 -            0x1028

"""


def test_a_line_runs_to_the_next_generated_row_and_keeps_the_code_it_inlined():
    ranges = parse(TABLE.splitlines())
    assert dict(ranges) == {("p_main_000.cpp", 5): [(0x1000, 0x10)], ("p_main_000.cpp", 6): [(0x1010, 8)],
                            ("p_main_000.cpp", 7): [(0x1020, 8)]}


def test_touching_ranges_merge():
    assert merge([(0x20, 4), (0x10, 0x10), (0x40, 2)]) == [(0x10, 0x14), (0x40, 2)]
