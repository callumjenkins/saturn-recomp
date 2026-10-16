"""Two recordings of the same sound compared window by window: the offset that lines them up, how alike
they are there, their levels and clipping, and whether the channels are the right way round.

Two sound implementations scheduled independently never give the same samples, so a window is judged by
its normalized correlation at the best offset rather than by equal bytes.
"""
import array
import math
import operator
import wave
from dataclasses import dataclass

RATE = 44100
SILENT = 1e-3          # RMS, as a fraction of full scale (-60 dBFS), below which a window is silence
DECIMATE = 8           # the coarse search's step, before the fine search at every sample


def read_wav(path):
    """(rate, left, right) of a 16-bit stereo WAV, each channel an array of ints."""
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 2:
            raise ValueError(f"{path}: not 16-bit stereo")
        raw = array.array("h", w.readframes(w.getnframes()))
        return w.getframerate(), raw[0::2], raw[1::2]


def write_wav(path, left, right, rate=RATE):
    both = array.array("h", bytes(4 * len(left)))
    both[0::2], both[1::2] = array.array("h", left), array.array("h", right)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(both.tobytes())


def stereo_bytes(raw):
    """(left, right) from interleaved 16-bit little-endian stereo, as a libretro core gives it."""
    both = array.array("h", raw[:len(raw) // 4 * 4])
    return both[0::2], both[1::2]


def rms(x):
    return math.sqrt(sum(map(operator.mul, x, x)) / len(x)) / 32768 if len(x) else 0.0


def db(level):
    return 20 * math.log10(level) if level > 0 else -math.inf


def correlation(a, b):
    """Normalized correlation of two equal-length sequences, about zero rather than their means."""
    ab = sum(map(operator.mul, a, b))
    aa = sum(map(operator.mul, a, a))
    bb = sum(map(operator.mul, b, b))
    return ab / math.sqrt(aa * bb) if aa and bb else 0.0


def best_lag(a, b, max_lag):
    """(lag, correlation) where b[max_lag + lag:][:len(a)] is most like a; len(b) is len(a) + 2 * max_lag.
    A coarse search on sums of DECIMATE samples, then every sample about the coarse best."""
    n, k = len(a), DECIMATE
    ca, cb = blocks(a, k), blocks(b[max_lag % k:], k)
    m = max_lag // k
    coarse = k * max(range(-m, m + 1), key=lambda lag: correlation(ca, cb[m + lag:m + lag + len(ca)]))
    lo, hi = max(-max_lag, coarse - DECIMATE), min(max_lag, coarse + DECIMATE)
    scored = [(correlation(a, b[max_lag + lag:max_lag + lag + n]), lag) for lag in range(lo, hi + 1)]
    corr, lag = max(scored)
    return lag, corr


def blocks(x, k):
    """Sums of k samples at a time: the signal low-passed, at 1/k the rate."""
    return [sum(x[i:i + k]) for i in range(0, len(x) - k + 1, k)]


def slope(points):
    """The least-squares slope of (x, y) points, or None for fewer than two distinct x."""
    n = len(points)
    if n < 2:
        return None
    mx, my = sum(x for x, _ in points) / n, sum(y for _, y in points) / n
    sxx = sum((x - mx) ** 2 for x, _ in points)
    return sum((x - mx) * (y - my) for x, y in points) / sxx if sxx else None


@dataclass
class Window:
    lag: int                 # samples the reference's sound comes after ours
    corr: float | None       # at the lag, of the channels summed; None when both are silent
    level: float             # ours, RMS of full scale
    level_ref: float
    clipped: int             # samples at full scale, ours
    clipped_ref: int
    swapped: bool            # our left is more like the reference's right than its left


def clipped(*channels):
    return sum(1 for ch in channels for s in ch if s >= 32767 or s <= -32768)


def window(ours, ref, max_lag):
    """A Window for ours = (left, right) against ref = (left, right), where ref has max_lag extra samples
    on each side of the span ours covers."""
    (l, r), (rl, rr) = ours, ref
    n = len(l)
    mono = [x + y for x, y in zip(l, r)]
    ref_mono = [x + y for x, y in zip(rl, rr)]
    mid = (ref_mono[max_lag:max_lag + n], rl[max_lag:max_lag + n], rr[max_lag:max_lag + n])
    level, level_ref = rms(mono) / 2, rms(mid[0]) / 2
    if level < SILENT and level_ref < SILENT:
        return Window(0, None, level, level_ref, clipped(l, r), clipped(mid[1], mid[2]), False)
    lag, corr = best_lag(mono, ref_mono, max_lag)
    at = slice(max_lag + lag, max_lag + lag + n)
    straight = correlation(l, rl[at]) + correlation(r, rr[at])
    crossed = correlation(l, rr[at]) + correlation(r, rl[at])
    return Window(lag, corr, level, level_ref, clipped(l, r), clipped(mid[1], mid[2]), crossed > straight + 0.2)
