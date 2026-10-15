import pytest
from conftest import assemble

from saturnrecomp.executed import classify, explain, fold, main, names, runs
from saturnrecomp.recomp.__main__ import generate

BASE = 0x06004000
A = bytes.fromhex("7001 000B 0009 7002")          # add #1,r0; rts; nop; add #2,r0
B = bytes.fromhex("E005 000B 0009")               # another program at the same base
MODULES = [("A", BASE, A), ("B", BASE, B)]
MAPPED = {"A": {BASE, BASE + 2, BASE + 4}, "B": {BASE, BASE + 2, BASE + 4}}


def sort(executed):
    out = classify({k: ["run.tsv"] for k in executed}, MODULES, MAPPED)
    return {c: [(m, a) for m, a, _ in rows] for c, rows in out.items()}


def test_the_bytes_executed_say_which_program_ran_and_whether_it_is_translated():
    found = sort([("M", BASE, 0x7001), ("S", BASE, 0xE005), ("M", BASE + 6, 0x7002)])
    assert found == {"translated": [("A", BASE), ("B", BASE)], "untranslated": [("A", BASE + 6)]}


def test_mirrors_fold_and_the_bios_and_other_bytes_are_their_own_classes():
    found = sort([("M", 0x26004000, 0x7001), ("M", 0x00080010, 0x1234), ("M", BASE, 0x1234),
                  ("M", 0x05A00000, 0x0009), ("68K", 0x1000, 0x4E75)])
    assert found == {"translated": [("A", BASE)], "BIOS": [("", 0x10)], "no module": [("", BASE)],
                     "elsewhere": [("", 0x05A00000)], "68K": [("", 0x1000)]}
    assert fold(0x060F0000 + 0x100000) == 0x060F0000


def test_runs_join_consecutive_halfwords_of_one_module():
    assert runs([("A", 0x10, ["x"]), ("A", 0x12, ["y"]), ("A", 0x20, ["x"])]) == [
        ("A", 0x10, 0x12, 2, {"x", "y"}), ("A", 0x20, 0x20, 1, {"x"})]


def test_a_note_explains_the_addresses_in_its_range():
    rows = [("", 0x060EC8B0, ["x"]), ("", 0x060EC8B2, ["x"]), ("", 0x06010000, ["x"])]
    by, rest = explain(rows, [(0x060EC8B0, 0x060F08AF, "the backup library")])
    assert dict(by) == {"the backup library": 2} and rest == [("", 0x06010000, ["x"])]


def test_runs_whose_files_share_a_name_go_by_their_directories():
    assert names(["a/boot/exec.tsv", "b/normal/exec.tsv", "c/x.tsv"]) == ["boot", "normal", "x.tsv"]


def test_a_function_discovery_missed_is_reported_once_the_reference_runs_it(tmp_path):
    # the entry calls through a pointer it is passed, so discovery cannot find the function at +0x0C
    code = assemble(0x6142, 0x410B, 0x0009, 0x000B, 0x0009, 0x0009, 0x7002, 0x000B, 0x0009)
    (tmp_path / "prog.bin").write_bytes(code)
    (tmp_path / "game.toml").write_text(f'[[module]]\nname = "PROG"\nfile = "prog.bin"\nbase = {BASE}\n')
    generate([("PROG", str(tmp_path / "prog.bin"), BASE, [BASE])], str(tmp_path / "build" / "recomp"),
             log=lambda s: None)
    ran = [(BASE, 0x6142), (BASE + 2, 0x410B), (BASE + 4, 0x0009), (BASE + 0xC, 0x7002),
           (BASE + 0xE, 0x000B), (BASE + 0x10, 0x0009), (BASE + 6, 0x000B), (BASE + 8, 0x0009)]
    (tmp_path / "run.tsv").write_text("".join(f"M\t{a:08X}\t{op:04X}\t1\t1\n" for a, op in ran))
    with pytest.raises(SystemExit) as stop:
        main([str(tmp_path / "game.toml"), str(tmp_path / "run.tsv")])
    assert stop.value.code == 1
    gaps = (tmp_path / "build" / "inventory" / "executed" / "untranslated.tsv").read_text().splitlines()[1:]
    assert gaps == [f"PROG\t{BASE + 0xC:08X}\t{BASE + 0x10:08X}\t3\trun.tsv"]
