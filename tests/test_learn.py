import types

from saturnrecomp import learn


def test_a_run_that_fails_says_so_in_its_log(tmp_path):
    log = learn.run(types.SimpleNamespace(saturn="false"), [], str(tmp_path / "out"))
    assert log.endswith(f"{learn.EXITED} 1\n")
    assert open(tmp_path / "out" / "log.txt").read() == log


def test_a_run_that_ends_well_adds_nothing(tmp_path):
    assert learn.run(types.SimpleNamespace(saturn="true"), [], str(tmp_path / "out")) == ""


def test_a_failed_exit_starts_its_own_line_with_its_status(tmp_path):
    script = tmp_path / "crash"
    script.write_text("#!/bin/sh\nprintf 'FATAL] partial' >&2\nexit 3\n")
    script.chmod(0o755)
    log = learn.run(types.SimpleNamespace(saturn=str(script)), [], str(tmp_path / "out"))
    assert log.splitlines()[-1] == f"{learn.EXITED} 3"
