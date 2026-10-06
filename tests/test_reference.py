from saturnrecomp import reference


def test_presses_use_the_runtimes_input_syntax():
    assert reference.presses("10:START,18:,20:2.A+B") == {10: [(0, {"START"})], 18: [(0, set())], 20: [(1, {"A", "B"})]}


def test_every_saturn_button_has_a_retropad_button_of_its_own():
    assert len(set(reference.RETROPAD.values())) == len(reference.RETROPAD) == 13
