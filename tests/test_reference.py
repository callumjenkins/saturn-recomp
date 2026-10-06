from saturnrecomp import agent, reference


def test_presses_use_the_runtimes_input_syntax():
    assert reference.presses("10:START,18:,20:2.A+B") == {10: [(0, {"START"})], 18: [(0, set())], 20: [(1, {"A", "B"})]}


def test_every_saturn_button_has_a_retropad_button_of_its_own():
    assert len(set(reference.RETROPAD.values())) == len(reference.RETROPAD) == 13


def test_words_come_back_in_the_saturns_byte_order():
    assert reference.swap16(b"\x34\x12\x78\x56") == b"\x12\x34\x56\x78"


def test_a_vblank_is_placed_by_its_tick_and_the_vblanks_since_it_began():
    ticks = {1: 0, 2: 1, 3: 1, 4: 1, 5: 2}
    assert reference.placed(ticks) == {1: (0, 0), 2: (1, 0), 3: (1, 1), 4: (1, 2), 5: (2, 0)}


class FakeCore:
    """A core whose tick is given frame by frame."""

    def __init__(self, ticks):
        self.ticks, self.frames, self.pads, self.seen = ticks, 0, [set(), set()], []

    def run(self):
        self.frames += 1
        self.seen.append(set(self.pads[0]))

    def read32(self, addr):
        return self.ticks[min(self.frames, len(self.ticks)) - 1]


def test_presses_and_shots_follow_the_tick_through_a_longer_stall():
    ours = {1: 5, 2: 6, 3: 6, 4: 7, 5: 8}                 # a stall of two VBlanks at tick 6
    core = FakeCore([1, 2, 3, 4, 5, 6, 6, 6, 6, 7, 8, 9])  # the core's lasts four
    shots = []
    end = reference.play_synced(core, 0, ours, {3: [(0, {"START"})], 5: [(0, set())]}, [4, 5], 100,
                                lambda v, n: shots.append((v, n)))
    assert shots == [(4, 10), (5, 11)]
    # held from the frame after the one its place falls on, as a press of ours holds from its VBlank on
    assert core.seen[6:] == [set(), {"START"}, {"START"}, {"START"}, {"START"}]
    assert end == 11


def test_a_place_the_core_never_stops_at_comes_on_its_next_frame():
    ours = {1: 1, 2: 1, 3: 1, 4: 2}                       # three VBlanks at tick 1
    core = FakeCore([1, 2, 3])                            # the core spends one
    shots = []
    reference.play_synced(core, 0, ours, {}, [3], 100, lambda v, n: shots.append((v, n)))
    assert shots == [(3, 2)]


def test_a_difference_counts_dots_inside_the_cores_frame_and_marks_them():
    ours = agent.Frame(2, 1, bytes([10, 10, 10, 20, 20, 20]))
    theirs = agent.Frame(4, 3, bytes(4 * 3 * 3))
    rgb = bytearray(theirs.rgb)
    rgb[(1 * 4 + 1) * 3:(1 * 4 + 3) * 3] = bytes([10, 10, 10, 20, 20, 26])
    n, worst, picture = reference.difference(ours, agent.Frame(4, 3, bytes(rgb)))
    assert (n, worst) == (1, 6)
    assert picture.width == 6 and picture.rgb[15:18] == b"\xff\x00\xff"
