import re

from conftest import BASE, assemble

from saturnrecomp.recomp.__main__ import generate

NOP, RTS = 0x0009, 0x000B
ADD1 = 0x7001                                    # add #1,r0


def rows(tmp_path, code, seeds=()):
    (tmp_path / "prog.bin").write_bytes(code)
    generate([("PROG", str(tmp_path / "prog.bin"), BASE, [BASE, *seeds])], str(tmp_path / "out"), log=lambda s: None)
    out = []
    for line in open(tmp_path / "out" / "map" / "PROG.tsv"):
        if not line.startswith("#"):
            a, f, kind, name, n = line.split()
            out.append((int(a, 16), int(f, 16), kind, name, int(n)))
    return out


def source(tmp_path, name, n):
    return open(tmp_path / "out" / name).read().splitlines()[n - 1]


def test_each_instruction_maps_to_the_line_that_implements_it(tmp_path):
    code = assemble(ADD1, ADD1, RTS, ADD1)
    found = rows(tmp_path, code)
    assert [(a, k) for a, _, k, _, _ in found] == [(BASE, "insn"), (BASE + 2, "insn"), (BASE + 4, "insn"),
                                                   (BASE + 6, "slot")]
    for a, _, kind, name, n in found:
        branch = a - 2 if kind == "slot" else a
        assert re.search(r"// %08X " % branch, source(tmp_path, name, n))


def test_an_instruction_two_functions_share_has_a_row_for_each(tmp_path):
    # 00: add, then into 02's code; 02 is an entry of its own
    code = assemble(ADD1, ADD1, RTS, NOP)
    found = rows(tmp_path, code, seeds=[BASE + 2])
    shared = sorted(f for a, f, _, _, _ in found if a == BASE + 2)
    assert shared == [BASE, BASE + 2]
    for a, f, kind, name, n in found:
        branch = a - 2 if kind == "slot" else a
        assert re.search(r"// %08X " % branch, source(tmp_path, name, n))
