import pytest

from saturnrecomp.android import plan_run

CUE = "/data/user/0/app/files/disc/game.cue"
WORK = "/data/user/0/app/files/adb-run"


def test_the_phone_plays_its_own_disc_into_its_own_folder():
    plan = plan_run(["--out", "build/run", "--cue", "/discs/game.cue", "--vblanks", "600"], CUE, WORK, lambda p: False)
    assert plan.args == ["--out", f"{WORK}/out", "--cue", CUE, "--vblanks", "600", "--log", f"{WORK}/log.txt"]
    assert plan.out == "build/run"
    assert plan.pushes == plan.pulls == []


def test_files_read_go_to_the_phone_and_files_written_come_back():
    here = {"inputs/route.txt", "build/start.bin"}
    plan = plan_run(["--input", "@inputs/route.txt", "--state-in", "build/start.bin", "--state-out", "build/end.bin",
                     "--wav", "build/sound.wav"], CUE, WORK, here.__contains__)
    assert plan.args[:8] == ["--input", f"@{WORK}/read/0-route.txt", "--state-in", f"{WORK}/read/1-start.bin",
                             "--state-out", f"{WORK}/written/0-end.bin", "--wav", f"{WORK}/written/1-sound.wav"]
    assert plan.pushes == [("inputs/route.txt", f"{WORK}/read/0-route.txt"), ("build/start.bin", f"{WORK}/read/1-start.bin")]
    assert plan.pulls == [(f"{WORK}/written/0-end.bin", "build/end.bin"), (f"{WORK}/written/1-sound.wav", "build/sound.wav")]


@pytest.mark.parametrize("save, pushed", [pytest.param("saves.bin", True, id="kept saves"),
                                          pytest.param("new.bin", False, id="saves yet to be made")])
def test_a_save_file_goes_both_ways(save, pushed):
    plan = plan_run(["--save", save], CUE, WORK, {"saves.bin"}.__contains__)
    phone = f"{WORK}/save/{save}"
    assert plan.args[:2] == ["--save", phone]
    assert plan.pushes == ([(save, phone)] if pushed else [])
    assert plan.pulls == [(phone, save)]


def test_no_saves_and_no_settings_stay_as_they_are():
    plan = plan_run(["--save", "-", "--settings", "-"], CUE, WORK, lambda p: True)
    assert plan.args[:4] == ["--save", "-", "--settings", "-"]
    assert plan.pushes == []


@pytest.mark.parametrize("arg", ["--agent", "--video", "--other-disc", "--log"])
def test_what_the_phone_cannot_take_is_refused(arg):
    with pytest.raises(ValueError, match=arg):
        plan_run([arg, "x"], CUE, WORK, lambda p: False)


def test_an_argument_with_a_newline_is_refused():
    with pytest.raises(ValueError, match="newline"):
        plan_run(["--input", "1:A\n2:"], CUE, WORK, lambda p: False)
