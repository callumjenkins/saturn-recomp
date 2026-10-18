"""Two recordings of the same sound compared window by window: the offset that lines them up, how alike
they are there, their levels and clipping, and whether the channels are the right way round. Within a
window that matches, its 10 ms parts are compared too, so a click or a dropout too short to move the
window shows. And a recording of what a sound device was given is lined up with the samples the run made.

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


PART = 441             # samples: 10 ms


@dataclass
class Part:
    start: int               # samples into the window
    corr: float              # 0 when one side is quieter than the floor
    level: float
    level_ref: float


def parts(ours, ref, max_lag, lag, floor, search=2):
    """The window's 10 ms parts where either side is louder than `floor` (RMS of full scale), each
    against the reference's at `lag`, give or take `search` samples. ours and ref are as window's."""
    (l, r), (rl, rr) = ours, ref
    mono = [x + y for x, y in zip(l, r)]
    ref_mono = [x + y for x, y in zip(rl, rr)]
    out = []
    for start in range(0, len(mono) - PART + 1, PART):
        a = mono[start:start + PART]
        at = max_lag + lag + start
        level, level_ref = rms(a) / 2, rms(ref_mono[at:at + PART]) / 2
        if level < floor and level_ref < floor:
            continue
        corr = 0.0 if level < floor or level_ref < floor else max(
            correlation(a, ref_mono[at + d:at + d + PART]) for d in range(-search, search + 1)
            if 0 <= at + d and at + d + PART <= len(ref_mono))
        out.append(Part(start, corr, level, level_ref))
    return out


@dataclass
class Live:
    offset: int | None       # frames the device's first sound comes after the run's; None when either is silent
    shifts: list             # (frame of the run's, frames): silence put in (+) or samples left out (-) there
    unmatched: list          # frames of the run's where no placement nearby matches
    compared: int            # frames of the run's found equal on the device, silence after the first sound included
    ended: int | None        # frame of the run's where the device's recording ran out, if it did


def live(run, device, block=PART, reach=RATE):
    """The device's recording, interleaved 16-bit stereo, against the run's own samples: where the device
    went silent for a while or skipped some, searching `reach` frames either way, and where it played
    something else. The run's silent stretches are passed over, since they line up anywhere."""
    def first(x):
        return next((i // 2 for i, s in enumerate(x) if s), None)

    def find(needle, hay, near):
        """The nearest d within reach where hay holds needle at frame near + d."""
        return next((d for d in sorted(range(-reach, reach + 1), key=abs)
                     if 0 <= near + d and 2 * (near + d) + len(needle) <= len(hay)
                     and hay[2 * (near + d):2 * (near + d) + len(needle)] == needle), None)

    w0, d0 = first(run), first(device)
    if w0 is None or d0 is None:
        return Live(None, [], [], 0, None)
    n, m, b = len(run) // 2, len(device) // 2, 2 * block
    quiet = array.array("h", bytes(b * 2))
    off, i, shifts, unmatched, compared = d0 - w0, w0, [], [], 0
    while i + block <= n:
        j = i + off
        if j + block > m:
            return Live(d0 - w0, shifts, unmatched, compared, i)
        mine = run[2 * i:2 * i + b]
        theirs = device[2 * j:2 * j + b] if j >= 0 else array.array("h")
        if theirs == mine:
            compared += block
            i += block
            continue
        if mine == quiet:
            i += block
            continue
        k = next((t // 2 for t in range(min(len(theirs), b)) if theirs[t] != mine[t]), 0)
        compared += k
        i, j = i + k, j + k
        mine = run[2 * i:2 * i + b]
        theirs = device[2 * j:2 * j + b] if j >= 0 else array.array("h")
        if mine == quiet:
            i += block
            continue
        put_in = find(mine, device, j)
        if put_in is not None and put_in > 0 and any(device[2 * j:2 * (j + put_in)]):
            put_in = None                         # what the device played before them is not silence put in
        left_out = None if theirs == quiet or len(theirs) < b else find(theirs, run, i)
        if left_out is not None and left_out > 0 and (put_in is None or left_out <= abs(put_in)):
            shifts.append((i, -left_out))
            i += left_out
            off -= left_out
        elif put_in is not None:
            shifts.append((i, put_in))
            off += put_in
        else:
            unmatched.append(i)
            i += block
    return Live(d0 - w0, shifts, unmatched, compared, None)
