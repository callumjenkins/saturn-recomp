import json
import os

from saturnrecomp import build, config

TOML = """
[game]
name = "Test"

[[module]]
name = "MAIN"
file = "main.bin"
base = 0x06004000
"""


def game(tmp_path, toml=TOML):
    (tmp_path / "game.toml").write_text(toml)
    if not (tmp_path / "main.bin").exists():
        (tmp_path / "main.bin").write_bytes(b"\x00\x09" * 8)
    return config.load(str(tmp_path / "game.toml"))


def built(tmp_path):
    """A game as a recompile leaves it: a binary, and the inputs it was made from."""
    g = game(tmp_path)
    os.makedirs(os.path.dirname(g.saturn))
    open(g.saturn, "w").close()
    os.makedirs(os.path.join(g.build, "recomp"))
    json.dump(build.inputs(g), open(build._stamp(g), "w"))
    return g


def test_an_unchanged_build_is_reused(tmp_path):
    assert build.stale(built(tmp_path)) == []


def test_no_build_yet(tmp_path):
    assert build.stale(game(tmp_path)) == ["no build yet"]


def test_a_build_without_a_record_is_stale(tmp_path):
    g = built(tmp_path)
    os.remove(build._stamp(g))
    assert build.stale(g) == ["no record of the last recompile's inputs"]


def test_a_changed_module_file_is_named(tmp_path):
    g = built(tmp_path)
    (tmp_path / "main.bin").write_bytes(b"\x00\x0b" * 8)
    assert build.stale(g) == ["module MAIN"]


def test_new_seeds_are_a_change(tmp_path):
    g = built(tmp_path)
    g.save_learned_seeds({"MAIN": [0x06004010]})
    assert build.stale(g) == ["seeds"]


def test_a_config_change_that_reaches_the_code_is_a_change(tmp_path):
    built(tmp_path)
    g = game(tmp_path, TOML + "\n[[hook]]\nmodule = \"MAIN\"\nat = 0x06004004\n")
    assert build.stale(g) == ["game.toml"]


def test_symbols_alone_are_no_change(tmp_path):
    built(tmp_path)
    assert build.stale(game(tmp_path, TOML + "\n[symbols]\nplayer = 0x060C0000\n")) == []


def test_the_recompilers_source_is_an_input(tmp_path, monkeypatch):
    g = built(tmp_path)
    fake = tmp_path / "pkg"
    fake.mkdir()
    (fake / "emit.py").write_text("# another recompiler\n")
    monkeypatch.setattr(build, "PACKAGE", str(fake))
    assert build.stale(g) == ["saturnrecomp"]
