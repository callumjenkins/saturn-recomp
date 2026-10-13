import hashlib
import json

from conftest import BASE, assemble

from saturnrecomp import config
from saturnrecomp.inventory import evidence, inventory
from saturnrecomp.recomp.__main__ import generate

ADD1, RTS, NOP = 0x7001, 0x000B, 0x0009
A = assemble(ADD1, RTS, NOP, 0x1234)             # code, then a halfword nothing reaches
B = assemble(RTS, ADD1)                          # another program at the same base


def game(tmp_path, treatments="", patch=False):
    disc = tmp_path / "disc"
    disc.mkdir()
    (disc / "A.BIN").write_bytes(A)
    (disc / "B.BIN").write_bytes(b"\xFF\xFF" + B)
    (disc / "MOVIE.CPK").write_bytes(b"cpk")
    prepared = bytearray(B)
    if patch:
        prepared[0:2] = assemble(ADD1)
    (tmp_path / "b.bin").write_bytes(prepared)
    files = {f"/{p.name}": {"sha1": hashlib.sha1(p.read_bytes()).hexdigest(), "size": p.stat().st_size}
             for p in disc.iterdir()}
    (tmp_path / "disc.json").write_text(json.dumps({"files": files, "audio": {"02": {"frames": 10, "sha1": "x"}}}))
    (tmp_path / "inventory.tsv").write_text(treatments)
    (tmp_path / "game.toml").write_text(f"""
[[module]]
name = "A"
file = "disc/A.BIN"
base = {BASE:#x}

[[module]]
name = "B"
file = "b.bin"
base = {BASE:#x}
source = "/B.BIN"
source_offset = 2

[inventory]
manifest = "disc.json"
files = "disc"
treatments = "inventory.tsv"
""")
    g = config.load(str(tmp_path / "game.toml"))
    generate([("A", g.module("A").file, BASE, [BASE]), ("B", g.module("B").file, BASE, [BASE])],
             str(tmp_path / "build" / "recomp"), log=lambda s: None)
    return g


def run(g):
    files, images, gaps, problems = inventory(g, log=lambda s: None)
    return {f[0]: f[3:5] for f in files}, {r["module"]: r for r in images}, gaps, problems


def test_modules_at_one_base_are_reported_apart_with_their_sources(tmp_path):
    files, images, gaps, problems = run(game(tmp_path))
    assert problems == []
    assert (images["A"]["source"], images["B"]["source"]) == ("/A.BIN", "/B.BIN+0x2")
    assert (images["A"]["instructions"], images["B"]["instructions"]) == (3, 2)
    assert files["/A.BIN"] == ("translated", "module A") and files["/B.BIN"] == ("translated", "module B")
    assert gaps == [("A", BASE + 6, 6, 2, "other", 0, 0, 0)]


def test_a_prepared_modules_changed_bytes_are_its_patches(tmp_path):
    _, images, _, _ = run(game(tmp_path, patch=True))
    assert images["B"]["patches"] == "0x0+2"


def test_files_no_module_comes_from_take_their_treatment_or_stay_unknown(tmp_path):
    files, *_ = run(game(tmp_path, "track *\tdata\tCD-DA\n"))
    assert files["track 02"] == ("data", "CD-DA")
    assert files["/MOVIE.CPK"] == ("unknown", "")


def test_an_instruction_with_no_generated_line_fails_the_check(tmp_path):
    g = game(tmp_path)
    path = tmp_path / "build" / "recomp" / "map" / "A.tsv"
    path.write_text("".join(line for line in open(path) if not line.startswith(f"{BASE + 2:08X}")))
    *_, problems = run(g)
    assert problems == [f"A: 1 discovered instructions have no generated line, first {BASE + 2:08X}"]


def test_a_gap_reports_its_content_prologues_and_the_pointers_into_it():
    data = b"\0" * 4 + b"Some text\0" + bytes.fromhex("0000 2FE6 4F22 6EF3 FFFF 0009")
    gaps = [(BASE, 4), (BASE + 4, 10), (BASE + 14, 8), (BASE + 22, 4)]
    found = evidence(data, BASE, gaps, {BASE + 4, BASE + 18, BASE + 20, BASE + 40})
    assert found == [("zero", 0, 0, 0), ("text", 0, 1, 0), ("other", 1, 2, 0), ("fill", 0, 0, 0)]


def test_a_ranged_treatment_splits_a_file_and_the_rest_keeps_the_files_own(tmp_path):
    files, *_ = run(game(tmp_path, "/MOVIE.CPK@0-1\tinterpreted\ta header\n/MOVIE.CPK\tdata\tthe rest\n"))
    assert files["/MOVIE.CPK@0x0-0x1"] == ("interpreted", "a header")
    assert files["/MOVIE.CPK@0x1-0x3"] == ("data", "the rest")


def test_a_file_is_named_by_a_program_or_by_a_page_the_program_names(tmp_path):
    from saturnrecomp.inventory import naming
    (tmp_path / "prog.bin").write_bytes(b"\x2f\xe6file://index.htm\0FONT%d\0")
    (tmp_path / "INDEX.HTM").write_bytes(b"<img src=pic12.gif> <a href=more.htm>")
    (tmp_path / "PIC12.GIF").write_bytes(b"GIF89a\x01\x02")
    m = config.Module("PROG", str(tmp_path / "prog.bin"), BASE)
    found = naming(["/INDEX.HTM", "/PIC12.GIF", "/FONT10B.BMP", "/OTHER.GIF"], [m], str(tmp_path))
    assert found == {"/INDEX.HTM": ["PROG:full"], "/PIC12.GIF": ["/INDEX.HTM:full"], "/FONT10B.BMP": ["PROG:letters"]}


def test_a_named_only_treatment_skips_files_nothing_names(tmp_path):
    from saturnrecomp.inventory import treatment
    files, *_ = run(game(tmp_path, "/MOVIE.CPK?named\tdata\tnamed\n"))
    assert files["/MOVIE.CPK"] == ("unknown", "")
    assert treatment("/MOVIE.CPK", [("/MOVIE.CPK?named", "data", "named")], named=True) == ("data", "named")


def test_a_gap_that_decodes_cleanly_to_a_return_counts_its_instructions():
    from saturnrecomp import sh2
    from saturnrecomp.inventory import descent
    from saturnrecomp.recomp import discover
    code = assemble(RTS, NOP,                                # 00, the program
                    0xE001, ADD1, ADD1, RTS, ADD1,           # 04, a leaf nothing calls
                    0xFFFF,                                  # 0E fill
                    ADD1, ADD1, 0xFFF0)                      # 10, after fill rather than a return: no boundary
    p = discover.Program(sh2.Image(code, BASE), [BASE])
    count = descent(p)
    assert (count(BASE + 4, 12), count(BASE + 0x10, 6)) == (5, 0)
