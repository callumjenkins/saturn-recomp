"""The setup check: what it reports with tools missing, and how it reads the platform and CMake's probe."""
from saturnrecomp import prereqs


def test_missing_tools_are_problems_with_the_install_command(monkeypatch):
    monkeypatch.setattr(prereqs.shutil, "which", lambda tool: None)
    monkeypatch.setattr(prereqs, "system", lambda: "fedora")
    problems = prereqs.check(log=lambda s: None)
    assert problems[0] == "missing cmake, ninja, clang++"
    assert "SDL3-devel" in problems[1]


def test_no_ffmpeg_is_only_a_note(monkeypatch):
    monkeypatch.setattr(prereqs.shutil, "which", lambda tool: None if tool == "ffmpeg" else "/usr/bin/" + tool)
    monkeypatch.setattr(prereqs, "probe", lambda: (None, "3.4.14"))
    notes = []
    assert prereqs.check(log=notes.append) == []
    assert any("no ffmpeg" in n for n in notes)


def test_no_sdl3_is_a_problem(monkeypatch):
    monkeypatch.setattr(prereqs.shutil, "which", lambda tool: "/usr/bin/" + tool)
    monkeypatch.setattr(prereqs, "probe", lambda: (None, None))
    assert any("SDL3 not found" in p for p in prereqs.check(log=lambda s: None))


def test_a_derived_distribution_gets_its_parents_packages(monkeypatch, tmp_path):
    release = tmp_path / "os-release"
    release.write_text('ID=pop\nID_LIKE="ubuntu debian"\n')
    real_open = open
    monkeypatch.setattr(prereqs.sys, "platform", "linux")
    monkeypatch.setattr("builtins.open", lambda p, *a, **k: real_open(release if p == "/etc/os-release" else p, *a, **k))
    assert prereqs.system() == "ubuntu"
