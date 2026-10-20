import pytest
from conftest import BASE

from saturnrecomp import config
from saturnrecomp.recomp.__main__ import generate

NOP, RTS = 0x0009, 0x000B
SECOND = BASE + 6


def recompile(tmp_path, replaces):
    code = b"".join(w.to_bytes(2, "big") for w in (0x7001, RTS, NOP, 0x7002, RTS, NOP))   # add #1,r0 / add #2,r0
    (tmp_path / "prog.bin").write_bytes(code)
    generate([("PROG", str(tmp_path / "prog.bin"), BASE, [BASE, SECOND])], str(tmp_path / "out"),
             log=lambda s: None, replaces=replaces)
    return tmp_path / "out"


def test_a_replaced_function_calls_the_game_layers_symbol_and_returns(tmp_path):
    out = recompile(tmp_path, {"PROG": {BASE: "decomp_first"}})
    body = (out / "p_prog_000.cpp").read_text()
    first = body.split("void f_%08X" % SECOND)[0]
    assert "::decomp_first(c);" in first and "c.pc = c.pr;" in first
    assert "add" not in first
    assert "void decomp_first(SH2Context& c);" in (out / "p_prog_funcs.h").read_text()


def test_the_functions_not_named_are_still_recompiled(tmp_path):
    out = recompile(tmp_path, {"PROG": {BASE: "decomp_first"}})
    second = (out / "p_prog_000.cpp").read_text().split("void f_%08X" % SECOND)[1]
    assert "decomp_first" not in second and "c.r[0]" in second


def test_a_replace_that_is_not_a_function_entry_is_an_error(tmp_path):
    with pytest.raises(SystemExit, match="not a function entry"):
        recompile(tmp_path, {"PROG": {BASE + 2: "decomp_first"}})


TOML = """
[game]
name = "Test"

[[module]]
name = "MAIN"
file = "main.bin"
base = 0x06001000

[[replace]]
module = "MAIN"
at = 0x06001010
symbol = "decomp_f06001010"
"""


def load(tmp_path, toml):
    (tmp_path / "game.toml").write_text(toml)
    (tmp_path / "main.bin").write_bytes(b"\x00\x09" * 16)
    return config.load(str(tmp_path / "game.toml"))


def test_a_replace_is_read_by_module_and_entry(tmp_path):
    assert load(tmp_path, TOML).replaces == {"MAIN": {0x06001010: "decomp_f06001010"}}


def test_a_replace_symbol_must_be_a_c_identifier(tmp_path):
    with pytest.raises(SystemExit, match="not a C identifier"):
        load(tmp_path, TOML.replace("decomp_f06001010", "bad name;"))


def test_a_replace_in_an_unknown_module_is_an_error(tmp_path):
    with pytest.raises(SystemExit, match="OTHER"):
        load(tmp_path, TOML.replace('module = "MAIN"\nat', 'module = "OTHER"\nat'))
