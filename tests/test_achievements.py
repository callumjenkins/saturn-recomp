import os

import pytest
from conftest import BASE

from saturnrecomp import build, config

TOML = """
[game]
name = "Test"

[symbols]
stage_world = 0x060BC190

[[module]]
name = "MAIN"
file = "main.bin"
base = 0x06004000

[[achievement]]
id = "clear-world-1"
title = "World 1"
description = "Clear the first world"
points = 10
trigger = "0xH{stage_world}=2_d0xH{stage_world}=1"
"""


def game(tmp_path, toml=TOML):
    (tmp_path / "game.toml").write_text(toml)
    (tmp_path / "main.bin").write_bytes(b"\x00\x0b\x00\x09")       # rts / nop
    return config.load(str(tmp_path / "game.toml"))


def recompiled(tmp_path, monkeypatch, toml=TOML):
    """modules.cpp as a recompile of the game writes it, short of running CMake."""
    g = game(tmp_path, toml)
    monkeypatch.setattr(build, "_run", lambda what, cmd, cwd: None)
    monkeypatch.setattr(build, "build", lambda game: None)
    monkeypatch.setattr(build.prereqs, "check", lambda log: [])
    monkeypatch.setattr(build, "specs", lambda game, log: [("MAIN", str(tmp_path / "main.bin"), BASE, [])])
    build.recompile(g, log=lambda s: None)
    return open(os.path.join(g.build, "recomp", "modules.cpp")).read()


def test_an_achievement_compiles_into_g_achievements_with_its_trigger(tmp_path, monkeypatch):
    modules = recompiled(tmp_path, monkeypatch)
    assert ('extern const SH2Achievement g_achievements[] = {{"clear-world-1", "World 1", "Clear the first world", 10, '
            '"0xH1bc190=2_d0xH1bc190=1"}};') in modules
    assert "extern const int g_nachievements = 1;" in modules
    assert 'extern const char g_game_title[] = "Test";' in modules


def test_a_title_reaches_the_cpp_as_its_utf8_bytes(tmp_path, monkeypatch):
    modules = recompiled(tmp_path, monkeypatch, TOML.replace('title = "World 1"', 'title = "Say \\"hi\\" \\\\ ?? caf\u00e9"'))
    assert '{"clear-world-1", "Say \\042hi\\042 \\134 \\077\\077 caf\\303\\251", ' in modules


@pytest.mark.parametrize("old, new", [
    pytest.param("points = 10", "points = 20", id="an achievement"),
    pytest.param('name = "Test"', 'name = "Other"', id="the game's name"),
])
def test_a_change_to_what_the_tables_are_made_from_is_a_change_to_the_build(tmp_path, old, new):
    before = build.inputs(game(tmp_path))
    after = build.inputs(game(tmp_path, TOML.replace(old, new)))
    assert before["game.toml"] != after["game.toml"]


@pytest.mark.parametrize("address, expanded", [
    pytest.param(0x060BC190, "1bc190", id="work RAM high"),
    pytest.param(0x00200010, "000010", id="work RAM low"),
    pytest.param(0x260BC190, "1bc190", id="work RAM high, cache-through"),
    pytest.param(0x061BC190, "1bc190", id="work RAM high's first mirror"),
    pytest.param(0x07FBC190, "1bc190", id="work RAM high's last mirror"),
    pytest.param(0x06000000, "100000", id="work RAM high's first byte"),
    pytest.param(0x002FFFFF, "0fffff", id="work RAM low's last byte"),
])
def test_a_symbol_in_a_trigger_expands_to_its_rcheevos_address(tmp_path, address, expanded):
    g = game(tmp_path, TOML.replace("0x060BC190", "0x%08X" % address))
    assert g.achievements[0].trigger == "0xH%s=2_d0xH%s=1" % (expanded, expanded)


def test_a_symbol_with_an_offset_expands_to_the_address_it_names(tmp_path):
    g = game(tmp_path, TOML.replace("{stage_world}=1", "{stage_world+0x10}=1"))
    assert g.achievements[0].trigger == "0xH1bc190=2_d0xH1bc1a0=1"


@pytest.mark.parametrize("address", [
    pytest.param(0x25E00000, id="VDP2 VRAM"),
    pytest.param(0x001FFFFF, id="below work RAM low"),
    pytest.param(0x00300000, id="above work RAM low"),
    pytest.param(0x05FFFFFF, id="below work RAM high"),
    pytest.param(0x08000000, id="above work RAM high's mirrors"),
])
def test_a_trigger_symbol_outside_work_ram_is_a_build_error(tmp_path, address):
    with pytest.raises(SystemExit, match="clear-world-1.*stage_world.*%08X.*outside work RAM" % address):
        game(tmp_path, TOML.replace("0x060BC190", "0x%08X" % address))


SECOND = """
[[achievement]]
id = "clear-world-1"
title = "Again"
description = "The same id"
points = 5
trigger = "0xH{stage_world}=3"
"""


@pytest.mark.parametrize("toml, error", [
    pytest.param(TOML + SECOND, "clear-world-1 is defined twice", id="a repeated id"),
    pytest.param(TOML.replace("points = 10", "points = 101"), "points.*0 to 100", id="points above 100"),
    pytest.param(TOML.replace("points = 10", "points = -1"), "points.*0 to 100", id="points below 0"),
    pytest.param(TOML.replace("points = 10", 'points = "10"'), "points.*0 to 100", id="points as a string"),
    pytest.param(TOML.replace('id = "clear-world-1"', 'id = "clear world"'), "id 'clear world'", id="an id with a space"),
    pytest.param(TOML.replace('title = "World 1"\n', ""), "clear-world-1 has no title", id="no title"),
    pytest.param(TOML.replace('description = "Clear the first world"\n', ""), "clear-world-1 has no description", id="no description"),
    pytest.param(TOML.replace('trigger = "0xH{stage_world}=2_d0xH{stage_world}=1"\n', ""), "clear-world-1 has no trigger", id="no trigger"),
    pytest.param(TOML.replace('title = "World 1"', "title = 5"), "clear-world-1: title 5 is not a string", id="a title as a number"),
    pytest.param(TOML.replace("points = 10", "points = true"), "points.*0 to 100", id="points as a boolean"),
    pytest.param(TOML.replace("points = 10", "points = 10.0"), "points.*0 to 100", id="points as a float"),
    pytest.param(TOML.replace('id = "clear-world-1"\n', ""), "id None", id="no id"),
    pytest.param(TOML.replace('id = "clear-world-1"', "id = 5"), "id 5", id="an id as a number"),
    pytest.param(TOML.replace("{stage_world}=1", "{stage_wrold}=1"), "no symbol named stage_wrold", id="an unknown symbol"),
])
def test_the_achievements_table_is_checked(tmp_path, toml, error):
    with pytest.raises(SystemExit, match=error):
        game(tmp_path, toml)


def test_the_ends_of_the_points_range_are_allowed(tmp_path):
    assert game(tmp_path, TOML.replace("points = 10", "points = 0")).achievements[0].points == 0
    assert game(tmp_path, TOML.replace("points = 10", "points = 100")).achievements[0].points == 100


def test_a_game_with_no_achievement_builds_an_empty_table(tmp_path, monkeypatch):
    toml = TOML[:TOML.index("[[achievement]]")]
    modules = recompiled(tmp_path, monkeypatch, toml)
    assert "extern const int g_nachievements = 0;" in modules
    assert "extern const SH2Achievement g_achievements[] = {{nullptr, nullptr, nullptr, 0, nullptr}};" in modules
