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
