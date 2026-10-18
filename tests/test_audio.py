import array
import math
import random

from saturnrecomp import audio


def noise(n, seed=1, scale=8000):
    rng = random.Random(seed)
    return [int(rng.gauss(0, scale)) for _ in range(n)]


def test_finds_the_offset_and_the_match():
    src = noise(6000)
    ours = (src[500:4500], [s // 2 for s in src[500:4500]])
    late = 37                                         # the reference's sound comes 37 samples later
    ref = [0] * late + src
    ref = ref[500 - 100:4500 + 100]
    w = audio.window(ours, (ref, [s // 2 for s in ref]), 100)
    assert w.lag == late and w.corr > 0.999 and not w.swapped


def test_a_quieter_reference_still_matches_at_its_own_level():
    src = noise(4200)
    ours = (src[100:4100], src[100:4100])
    quiet = [s // 4 for s in src]
    w = audio.window(ours, (quiet, quiet), 100)
    assert w.lag == 0 and w.corr > 0.999
    assert round(audio.db(w.level) - audio.db(w.level_ref)) == 12


def test_swapped_channels():
    left, right = noise(4200, 1), noise(4200, 2)
    w = audio.window((left[100:4100], right[100:4100]), (right, left), 100)
    assert w.swapped


def test_silence_on_both_sides_has_no_correlation():
    assert audio.window(([0] * 1000, [0] * 1000), ([0] * 1200, [0] * 1200), 100).corr is None


def test_unrelated_sound_scores_low():
    w = audio.window((noise(4000, 3), noise(4000, 4)), (noise(4200, 5), noise(4200, 6)), 100)
    assert abs(w.corr) < 0.2


def test_counts_full_scale_samples():
    tone = [int(40000 * math.sin(i / 7)) for i in range(1200)]
    tone = [max(-32768, min(32767, s)) for s in tone]
    w = audio.window((tone[100:1100], tone[100:1100]), (tone, tone), 100)
    assert w.clipped > 0 and w.clipped == w.clipped_ref


def test_wav_round_trip(tmp_path):
    left, right = noise(500, 7), noise(500, 8)
    audio.write_wav(str(tmp_path / "a.wav"), left, right)
    rate, l, r = audio.read_wav(str(tmp_path / "a.wav"))
    assert rate == audio.RATE and list(l) == left and list(r) == right



def test_a_click_too_short_to_move_the_window_shows_in_its_part():
    src = noise(audio.RATE + 200, scale=2000)
    ours = src[100:100 + audio.RATE]
    ours[audio.PART * 5 + 30:audio.PART * 5 + 60] = [32767] * 30
    w = audio.window((ours, ours), (src, src), 100)
    assert w.corr > 0.9
    parts = audio.parts((ours, ours), (src, src), 100, w.lag, audio.SILENT)
    assert len(parts) == 100
    assert [p.start for p in parts if p.corr < 0.5] == [audio.PART * 5]


def test_a_part_silent_on_one_side_only_scores_zero_and_one_silent_on_both_is_left_out():
    src = noise(audio.PART * 3 + 200)
    ours = src[100:100 + audio.PART * 3]
    ref = src[:]
    ours[:audio.PART] = [0] * audio.PART                                     # ours misses the first
    ours[audio.PART * 2:] = ref[100 + audio.PART * 2:100 + audio.PART * 3] = [0] * audio.PART
    parts = audio.parts((ours, ours), (ref, ref), 100, 0, audio.SILENT)
    assert [(p.start, round(p.corr, 3)) for p in parts] == [(0, 0.0), (audio.PART, 1.0)]


def interleave(left, right):
    both = array.array("h", bytes(4 * len(left)))
    both[0::2], both[1::2] = array.array("h", left), array.array("h", right)
    return both


def made(frames=audio.PART * 20, seed=9):
    return interleave(noise(frames, seed, 4000), noise(frames, seed + 1, 4000))


def test_a_device_that_played_every_sample_after_its_lead_matches_throughout():
    run = made()
    played = audio.live(run, array.array("h", bytes(4 * 2205)) + run)
    assert (played.offset, played.shifts, played.unmatched, played.ended) == (2205, [], [], None)
    assert played.compared == len(run) // 2


def test_silence_put_in_and_samples_left_out_are_found_where_they_happened():
    run = made()
    at, skip = 3000, 5000
    device = run[:2 * at] + array.array("h", bytes(4 * 2300)) + run[2 * at:2 * skip] + run[2 * (skip + 700):]
    played = audio.live(run, device)
    assert played.shifts == [(at, 2300), (skip, -700)] and played.unmatched == []
    assert len(run) // 2 - 700 - audio.PART <= played.compared <= len(run) // 2 - 700


def test_samples_the_run_never_made_are_unmatched_and_a_short_recording_ends():
    run = made()
    device = run[:]
    device[2 * 4000:2 * 4000 + 2 * audio.PART] = array.array("h", [s // 2 for s in run[2 * 4000:2 * 4000 + 2 * audio.PART]])
    played = audio.live(run, device[:2 * 7000])
    assert played.unmatched == [4000] and played.ended is not None and 7000 - audio.PART < played.ended <= 7000


def test_a_silent_run_has_no_offset():
    assert audio.live(array.array("h", bytes(400)), made(100)).offset is None


def test_a_part_below_the_floor_on_one_side_only_scores_zero():
    src = noise(audio.PART + 200, scale=2000)
    ours = src[100:100 + audio.PART]
    faint = [s // 1000 for s in src]                                         # 60 dB down, far below the floor
    assert [p.corr for p in audio.parts((ours, ours), (faint, faint), 100, 0, audio.SILENT * 3)] == [0.0]


def test_parts_line_up_at_the_windows_lag_give_or_take_two_samples():
    src = noise(audio.PART * 4 + 400)
    ours = src[200:200 + audio.PART * 4]
    ref = [0] * 37 + src                                                     # the reference's sound 37 samples later
    ref = ref[100:100 + audio.PART * 4 + 200]
    parts = audio.parts((ours, ours), (ref, ref), 100, 35, audio.SILENT)    # the window's lag two short
    assert len(parts) == 4 and min(p.corr for p in parts) > 0.999


def test_a_part_gives_both_levels():
    src = noise(audio.PART + 200, scale=2000)
    ours = src[100:100 + audio.PART]
    quiet = [s // 4 for s in src]
    [part] = audio.parts((ours, ours), (quiet, quiet), 100, 0, audio.SILENT)
    assert round(audio.db(part.level) - audio.db(part.level_ref)) == 12
    assert abs(part.level - audio.rms(ours)) < 1e-3


def silent_middle():
    """A run with a second of silence after its first 3000 frames."""
    return made(3000, 3) + array.array("h", bytes(4 * audio.RATE)) + made(3000, 5)


def test_silence_put_in_during_the_runs_own_silence_is_found_where_its_sound_comes_back():
    run = silent_middle()
    at = 3000 + 20000
    device = run[:2 * at] + array.array("h", bytes(4 * 2300)) + run[2 * at:]
    played = audio.live(run, device)
    assert (played.shifts, played.unmatched, played.ended) == ([(3000 + audio.RATE, 2300)], [], None)


def test_silence_put_in_just_before_the_runs_own_silence_is_not_a_skip():
    run = silent_middle()
    at = 2900
    device = run[:2 * at] + array.array("h", bytes(4 * 2300)) + run[2 * at:]
    played = audio.live(run, device)
    assert played.unmatched == [] and all(shift > 0 for _, shift in played.shifts)


def test_samples_left_out_of_the_runs_own_silence_are_found_when_its_sound_comes_back():
    run = silent_middle()
    device = run[:2 * 20000] + run[2 * 22000:]
    played = audio.live(run, device)
    [(at, shift)] = played.shifts
    assert shift == -2000 and 3000 + audio.RATE - audio.PART <= at <= 3000 + audio.RATE and played.unmatched == []


def test_frames_the_device_played_twice_are_not_silence_put_in():
    run = made()
    at = 3000
    device = run[:2 * at] + run[2 * (at - 2300):2 * at] + run[2 * at:]
    played = audio.live(run, device)
    assert played.unmatched and not any(shift == 2300 for _, shift in played.shifts)


def test_a_silent_device_has_no_offset():
    assert audio.live(made(), array.array("h", bytes(4000))).offset is None
