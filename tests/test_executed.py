from saturnrecomp.executed import classify, fold, runs

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
