import pytest

from saturnrecomp import config

TOML = """
[game]
name = "Test"

[symbols]
sj = 0x06001000
lj = 0x06001020
hit = 0x06001100

[[module]]
name = "MAIN"
file = "main.bin"
base = 0x06001000
seeds = ["hit", 0x06001200]

[tasks]
module = "MAIN"
setjmp = "sj"
longjmp = "lj"
yields = ["sj"]

[[hook]]
module = "MAIN"
at = "hit"
"""


def load(tmp_path, text):
    path = tmp_path / "game.toml"
    path.write_text(text)
    return config.load(str(path))


def test_names_resolve_and_paths_are_relative_to_the_file(tmp_path):
    g = load(tmp_path, TOML)
    m = g.module("MAIN")
    assert m.file == str(tmp_path / "main.bin")
    assert m.seeds == [0x06001100, 0x06001200]
    assert (g.tasks.setjmp, g.tasks.longjmp, g.tasks.yields) == (0x06001000, 0x06001020, [0x06001000])


def test_the_task_switch_is_hooked_along_with_the_hooks(tmp_path):
    assert load(tmp_path, TOML).hooks == {"MAIN": [0x06001000, 0x06001020, 0x06001100]}


def test_learned_seeds_round_trip(tmp_path):
    g = load(tmp_path, TOML)
    assert g.learned_seeds() == {}
    g.save_learned_seeds({"MAIN": [0x06001300]})
    assert g.learned_seeds() == {"MAIN": [0x06001300]}


def test_an_unknown_symbol_is_an_error(tmp_path):
    with pytest.raises(SystemExit, match="no symbol named nope"):
        load(tmp_path, TOML.replace('at = "hit"', 'at = "nope"'))


def test_a_hook_in_an_unknown_module_is_an_error(tmp_path):
    with pytest.raises(SystemExit, match="OTHER"):
        load(tmp_path, TOML.replace('module = "MAIN"\nat', 'module = "OTHER"\nat'))


CHECKPOINT = """
[checkpoint]
keep = [["hit", 2], ["sj+0x10", 2], ["lj", 1]]
accept = [[0x0001, 0x00FE], [0x0200, 0x0200]]
presses = ["100:START", "110:"]
writes = ["50:sj=hit", "60:sj+0x10", "60:lj&0F"]
resume = 120
fresh = [0x0000]
show = [["Stage {}", "hit", "world-stage"], ["{} pts", "sj+0x10", "u16"], ["Speed {}", "lj", "steps:04:01?"],
        ["{}", "lj", "bits:01=Kick,02=Glove?"], ["{} dino", "lj", "names:,,,,,,Pink?", "lj&04"]]
"""


def test_a_checkpoint_rebuilds_the_writes_from_a_progress_line(tmp_path):
    cp = load(tmp_path, TOML + CHECKPOINT).checkpoint
    assert cp.keep_arg == "06001100:2,06001010:2,06001020:1"
    assert cp.rebuild("900 0003 ABCD 5A") == ["50:06001000=0003", "60:06001010=ABCD", "60:06001020=0A"]


def test_a_checkpoint_rebuilds_only_the_values_it_accepts(tmp_path):
    cp = load(tmp_path, TOML + CHECKPOINT).checkpoint
    assert cp.rebuild("900 0000 ABCD 5A") is None
    assert cp.rebuild("900 00FF ABCD 5A") is None
    assert cp.rebuild("900 0200 ABCD 5A") is not None


def test_a_checkpoint_shows_a_progress_line_in_words(tmp_path):
    cp = load(tmp_path, TOML + CHECKPOINT).checkpoint
    assert cp.fresh == [0]
    assert cp.describe("900 0103 0AF0 04") == ["Stage 2-4", "2,800 pts"]
    assert cp.describe("900 0000 0001 03") == ["Stage 1-1", "1 pts", "Speed -1", "Kick, Glove"]
    assert cp.describe("900 0000 0001 06") == ["Stage 1-1", "1 pts", "Speed +2", "Glove", "Pink dino"]
    assert cp.describe("900 0000 0001 05") == ["Stage 1-1", "1 pts", "Speed +1", "Kick"]
    assert cp.describe("900 0000 0001 0005") == []              # another keep's line
    assert config.show_value("u8@3", bytes.fromhex("E0000001")) == "1"
    assert cp.rebuild("900 0003 ABCD 0005") is None
    assert cp.to_json()["show"][4] == ["{} dino", 2, "names:,,,,,,Pink?", [2, 4]]
