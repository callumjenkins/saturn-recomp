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

