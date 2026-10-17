import pytest
from conftest import assemble, sound_program

from saturnrecomp import m68k
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
                  ("M", 0x05A00000, 0x0009)])
    assert found == {"translated": [("A", BASE)], "BIOS": [("", 0x10)], "no module": [("", BASE)],
                     "elsewhere": [("", 0x05A00000)]}
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


def test_68000_addresses_are_placed_among_the_sound_programs_by_their_bytes(m68k_lib):
    p = m68k.Program(m68k.Image(sound_program(0x4E75, 0x7001), m68k_lib))      # rts, then a word nothing reaches
    executed = [("68K", 0x400, 0x4E75), ("68K", 0x402, 0x7001), ("68K", 0x404, 0x4E75), ("68K", 0x80400, 0x4E75)]
    out = classify({k: ["run.tsv"] for k in executed}, MODULES, MAPPED, [("SND", p)])
    assert {c: [(m, a) for m, a, _ in rows] for c, rows in out.items()} == {
        "68K interpreted": [("SND", 0x400), ("SND", 0x400)], "68K undiscovered": [("SND", 0x402)],
        "68K no image": [("", 0x404)]}


def test_an_undiscovered_68000_instruction_fails_the_check_and_a_noted_one_does_not(tmp_path):
    disc = tmp_path / "disc"
    disc.mkdir()
    (disc / "SND.BIN").write_bytes(sound_program(0x4E75, 0x7001))
    (tmp_path / "game.toml").write_text('[inventory]\nmanifest = "disc.json"\nfiles = "disc"\n'
                                        '[[inventory.sound]]\nname = "SND"\nsource = "/SND.BIN"\n')
    (tmp_path / "notes.tsv").write_text("00000404\t00000405\tthe BIOS's program\n")
    run = tmp_path / "run.tsv"
    args = [str(tmp_path / "game.toml"), str(run), "--notes", str(tmp_path / "notes.tsv")]
    run.write_text("68K\t00000400\t4E75\n68K\t00000404\t1234\n")
    with pytest.raises(SystemExit) as stop:
        main(args)
    assert stop.value.code == 0
    run.write_text("68K\t00000402\t7001\n")
    with pytest.raises(SystemExit) as stop:
        main(args)
    assert stop.value.code == 1
    gaps = (tmp_path / "build" / "inventory" / "executed" / "68K-undiscovered.tsv").read_text().splitlines()[1:]
    assert gaps == ["SND\t00000402\t00000402\t1\trun.tsv"]


def test_an_interpreted_68000_address_names_the_program_whose_descent_found_it(m68k_lib):
    a = m68k.Program(m68k.Image(sound_program(0x4E75, 0x4E75), m68k_lib))     # 402 unreached
    b = m68k.Program(m68k.Image(sound_program(0x4E71, 0x4E75), m68k_lib))     # 402 reached
    out = classify({("68K", 0x402, 0x4E75): ["run.tsv"]}, MODULES, MAPPED, [("A", a), ("B", b)])
    assert [(m, x) for m, x, _ in out["68K interpreted"]] == [("B", 0x402)]


def test_other_bytes_at_a_programs_address_are_no_image_and_shared_bytes_name_both(m68k_lib):
    a = m68k.Program(m68k.Image(sound_program(0x4E75, 0x7001), m68k_lib))
    b = m68k.Program(m68k.Image(sound_program(0x4E75, 0x7001), m68k_lib))
    executed = [("68K", 0x400, 0x1234), ("68K", 0x402, 0x7001)]
    out = classify({k: ["run.tsv"] for k in executed}, MODULES, MAPPED, [("A", a), ("B", b)])
    assert {c: [(m, x) for m, x, _ in rows] for c, rows in out.items()} == {
        "68K no image": [("", 0x400)], "68K undiscovered": [("A/B", 0x402)]}


def test_an_unexplained_68000_address_fails_the_check_and_interpreted_ones_get_no_list(tmp_path):
    disc = tmp_path / "disc"
    disc.mkdir()
    (disc / "SND.BIN").write_bytes(sound_program(0x4E75))
    (tmp_path / "game.toml").write_text('[inventory]\nmanifest = "disc.json"\nfiles = "disc"\n'
                                        '[[inventory.sound]]\nname = "SND"\nsource = "/SND.BIN"\n')
    (tmp_path / "run.tsv").write_text("68K\t00000400\t4E75\n68K\t00000400\t1234\n")
    with pytest.raises(SystemExit) as stop:
        main([str(tmp_path / "game.toml"), str(tmp_path / "run.tsv")])
    assert stop.value.code == 1
    out = tmp_path / "build" / "inventory" / "executed"
    assert (out / "68K-no-image.tsv").exists() and not (out / "68K-interpreted.tsv").exists()
