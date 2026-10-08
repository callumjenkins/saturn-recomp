"""A libretro core played headless, frame by frame: the reference a recompiled run is compared with.

    python -m saturnrecomp.reference CORE.so --bios DIR --cue GAME.cue --frames N --shot F,... --out DIR
        [--input FRAME:BUTTONS,...] [--pads N]

Built for Beetle Saturn (Mednafen's Saturn), which runs the real BIOS: `--bios` is the folder holding
it (mpr-17933.bin for a US or European disc). Presses use the runtime's --input syntax, with frames
counted from the core's first; one retro_run is one frame. Pictures are saved as the core gives
them, at its own size: no scaling, and the options below turn off its cropping and blending.
"""
import argparse
import ctypes as C
import os

from . import agent

# libretro.h
ENV_GET_CAN_DUPE = 3
ENV_GET_SYSTEM_DIRECTORY = 9
ENV_SET_PIXEL_FORMAT = 10
ENV_GET_VARIABLE = 15
ENV_GET_VARIABLE_UPDATE = 17
ENV_GET_SAVE_DIRECTORY = 31
ENV_GET_CORE_OPTIONS_VERSION = 52
PIXEL_0RGB1555, PIXEL_XRGB8888, PIXEL_RGB565 = 0, 1, 2
DEVICE_NONE, DEVICE_JOYPAD = 0, 1
MEMORY_SYSTEM_RAM = 2

# Beetle Saturn's control pad as a RetroPad (its input.c): the RetroPad button each Saturn button is read from.
RETROPAD = {"B": 8, "C": 11, "A": 0, "START": 3, "UP": 4, "DOWN": 5, "LEFT": 6, "RIGHT": 7,
            "Z": 10, "Y": 9, "X": 1, "R": 13, "L": 12}

OPTIONS = {
    "beetle_saturn_initial_scanline": "0", "beetle_saturn_last_scanline": "239",
    "beetle_saturn_horizontal_overscan": "0", "beetle_saturn_horizontal_blend": "disabled",
    "beetle_saturn_midsync": "disabled", "beetle_saturn_jit_scu": "disabled", "beetle_saturn_jit_scsp": "disabled",
    "beetle_saturn_cdimagecache": "enabled",
    # with no time set, the BIOS stops at its clock screen; the host's time lets it boot straight on
    "beetle_saturn_autortc": "enabled",
}


class GameInfo(C.Structure):
    _fields_ = [("path", C.c_char_p), ("data", C.c_void_p), ("size", C.c_size_t), ("meta", C.c_char_p)]


class Variable(C.Structure):
    _fields_ = [("key", C.c_char_p), ("value", C.c_char_p)]


class Geometry(C.Structure):
    _fields_ = [("base_width", C.c_uint), ("base_height", C.c_uint), ("max_width", C.c_uint),
                ("max_height", C.c_uint), ("aspect_ratio", C.c_float)]


class Timing(C.Structure):
    _fields_ = [("fps", C.c_double), ("sample_rate", C.c_double)]


class AvInfo(C.Structure):
    _fields_ = [("geometry", Geometry), ("timing", Timing)]


ENVIRONMENT = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
VIDEO = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
AUDIO = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AUDIO_BATCH = C.CFUNCTYPE(C.c_size_t, C.POINTER(C.c_int16), C.c_size_t)
INPUT_POLL = C.CFUNCTYPE(None)
INPUT_STATE = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)


WORK_RAM = ((0x06000000, 0x100000), (0x00200000, 0))     # each bank's Saturn address and its offset in the core's


class Core:
    """One libretro core with one game loaded. `pads[port]` is the set of Saturn buttons held.
    Pads are plugged into the first `ports` ports only, as in the runtime: games ask different
    questions with a second pad connected."""

    def __init__(self, so, system_dir, game, options=OPTIONS, save_dir=None, ports=1):
        self.lib = C.CDLL(os.path.abspath(so))
        self.options = {k.encode(): v.encode() for k, v in options.items()}
        self._system = C.c_char_p(os.path.abspath(system_dir).encode())
        self._save = C.c_char_p(os.path.abspath(save_dir or system_dir).encode())
        self.pixel_format = PIXEL_0RGB1555
        self._raw = None                         # the last picture as the core gave it
        self.audio = bytearray()                 # 16-bit stereo, little-endian, as the core gave it
        self.pads = [set() for _ in range(12)]
        self.frames = 0
        # kept on self: the core holds these pointers for as long as it runs
        self._cbs = [ENVIRONMENT(self._environment), VIDEO(self._video), AUDIO(self._sample),
                     AUDIO_BATCH(self._batch), INPUT_POLL(lambda: None), INPUT_STATE(self._input)]
        L = self.lib
        L.retro_set_environment(self._cbs[0])
        L.retro_init()
        L.retro_set_video_refresh(self._cbs[1])
        L.retro_set_audio_sample(self._cbs[2])
        L.retro_set_audio_sample_batch(self._cbs[3])
        L.retro_set_input_poll(self._cbs[4])
        L.retro_set_input_state(self._cbs[5])
        self._game = GameInfo(os.path.abspath(game).encode(), None, 0, None)
        L.retro_load_game.restype = C.c_bool
        if not L.retro_load_game(C.byref(self._game)):
            raise RuntimeError(f"the core would not load {game}")
        for port in range(2):
            L.retro_set_controller_port_device(port, DEVICE_JOYPAD if port < ports else DEVICE_NONE)
        self.av = AvInfo()
        L.retro_get_system_av_info(C.byref(self.av))
        L.retro_get_memory_data.restype = C.c_void_p
        L.retro_get_memory_size.restype = C.c_size_t

    def _environment(self, cmd, data):
        if cmd == ENV_GET_SYSTEM_DIRECTORY:
            C.cast(data, C.POINTER(C.c_char_p))[0] = self._system
        elif cmd == ENV_GET_SAVE_DIRECTORY:
            C.cast(data, C.POINTER(C.c_char_p))[0] = self._save
        elif cmd == ENV_SET_PIXEL_FORMAT:
            self.pixel_format = C.cast(data, C.POINTER(C.c_int))[0]
        elif cmd == ENV_GET_VARIABLE:
            v = C.cast(data, C.POINTER(Variable))[0]
            value = self.options.get(v.key)
            if value is None:
                return False
            C.cast(data, C.POINTER(Variable))[0].value = value
        elif cmd == ENV_GET_VARIABLE_UPDATE:
            C.cast(data, C.POINTER(C.c_bool))[0] = False
        elif cmd == ENV_GET_CAN_DUPE:
            C.cast(data, C.POINTER(C.c_bool))[0] = True
        elif cmd == ENV_GET_CORE_OPTIONS_VERSION:
            C.cast(data, C.POINTER(C.c_uint))[0] = 0
        else:
            return False
        return True

    def _video(self, data, w, h, pitch):
        if data:                                 # none: a duplicate of the last frame
            self._raw = (C.string_at(data, pitch * h), w, h, pitch, self.pixel_format)

    @property
    def frame(self):
        """The last picture as an agent.Frame, decoded from the core's pixel format when asked for."""
        if self._raw is None:
            return None
        raw, w, h, pitch, fmt = self._raw
        rgb = bytearray(w * h * 3)
        for y in range(h):
            row = raw[y * pitch:]
            for x in range(w):
                if fmt == PIXEL_XRGB8888:
                    r, g, b = row[4 * x + 2], row[4 * x + 1], row[4 * x]
                else:
                    p = row[2 * x] | row[2 * x + 1] << 8
                    if fmt == PIXEL_RGB565:
                        r, g, b = (p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3
                        r, g, b = r | r >> 5, g | g >> 6, b | b >> 5
                    else:
                        r, g, b = ((p >> 10) & 31) << 3, ((p >> 5) & 31) << 3, (p & 31) << 3
                        r, g, b = r | r >> 5, g | g >> 5, b | b >> 5
                rgb[(y * w + x) * 3:(y * w + x) * 3 + 3] = bytes((r, g, b))
        return agent.Frame(w, h, bytes(rgb))

    def _sample(self, left, right):
        self.audio += left.to_bytes(2, "little", signed=True) + right.to_bytes(2, "little", signed=True)

    def _batch(self, data, frames):
        self.audio += C.string_at(data, frames * 4)
        return frames

    def _input(self, port, device, index, id):
        if device != DEVICE_JOYPAD or port >= len(self.pads):
            return 0
        return int(any(RETROPAD.get(b) == id for b in self.pads[port]))

    def run(self):
        self.lib.retro_run()
        self.frames += 1

    def ram(self):
        """The core's system RAM, as it exposes it."""
        size = self.lib.retro_get_memory_size(MEMORY_SYSTEM_RAM)
        at = self.lib.retro_get_memory_data(MEMORY_SYSTEM_RAM)
        return C.string_at(at, size) if at and size else b""

    def read(self, addr, n):
        """n bytes of work RAM at a Saturn address, as the Saturn sees them. Beetle Saturn exposes the
        low bank then the high one, as 16-bit words in the host's (little-endian) byte order."""
        base, at = next(((b, o) for b, o in WORK_RAM if b <= addr and addr + n <= b + 0x100000), (None, None))
        if base is None:
            raise ValueError(f"{addr:08X}+{n} is not in a work RAM")
        start = at + addr - base
        lo, hi = start & ~1, (start + n + 1) & ~1
        raw = C.string_at(self.lib.retro_get_memory_data(MEMORY_SYSTEM_RAM) + lo, hi - lo)
        return swap16(raw)[start - lo:start - lo + n]

    def write(self, addr, data):
        """Sets work RAM at a Saturn address, as --write does on ours."""
        base, at = next(((b, o) for b, o in WORK_RAM if b <= addr and addr + len(data) <= b + 0x100000), (None, None))
        if base is None:
            raise ValueError(f"{addr:08X}+{len(data)} is not in a work RAM")
        start = at + addr - base
        lo, hi = start & ~1, (start + len(data) + 1) & ~1
        mem = self.lib.retro_get_memory_data(MEMORY_SYSTEM_RAM)
        words = bytearray(swap16(C.string_at(mem + lo, hi - lo)))
        words[start - lo:start - lo + len(data)] = data
        C.memmove(mem + lo, swap16(bytes(words)), hi - lo)

    def read32(self, addr):
        return int.from_bytes(self.read(addr, 4), "big")

    def close(self):
        self.lib.retro_unload_game()
        self.lib.retro_deinit()


def swap16(raw):
    out = bytearray(raw)
    out[0::2], out[1::2] = raw[1::2], raw[0::2]
    return bytes(out)


def placed(ticks):
    """{VBlank: (tick, VBlanks since the tick reached that value)} for our run, from its VBlank: tick.
    A game's tick stops while it loads, and a core's loads take longer than ours, so an event placed
    this way happens at the same point of the game on both."""
    out, since, last = {}, 0, None
    for v in sorted(ticks):
        since = since + 1 if ticks[v] == last else 0
        last = ticks[v]
        out[v] = (last, since)
    return out


def play_synced(core, tick_addr, ticks, presses_by_vblank, shots, limit, on_shot, writes_by_vblank=None):
    """Plays our run's presses and writes ({VBlank: [(addr, bytes)]}) on the core, each at its place by
    the game's tick (`placed`), and calls
    on_shot(our VBlank, core frame number) where each of `shots` falls. A place the core passes
    without stopping at, a tick it skips or a stall it ends sooner, comes on its next frame. Returns
    the frames run, or None if `limit` ran out first."""
    at = placed(ticks)
    events = sorted([(at[v], v, 0, what) for v, what in presses_by_vblank.items() if v in at]
                    + [(at[v], v, 1, what) for v, what in (writes_by_vblank or {}).items() if v in at]
                    + [(at[v], v, 2, None) for v in shots if v in at])
    j, last, since = 0, None, 0
    while j < len(events):
        if core.frames >= limit:
            return None
        core.run()
        t = core.read32(tick_addr)
        since = since + 1 if t == last else 0
        last = t
        while j < len(events) and (events[j][0][0] < t or (events[j][0][0] == t and since >= events[j][0][1])):
            _, v, kind, what = events[j]
            if kind == 0:
                for port, buttons in what:
                    core.pads[port] = buttons
            elif kind == 1:
                for addr, data in what:
                    core.write(addr, data)
            else:
                on_shot(v, core.frames)
            j += 1
    return core.frames


def difference(ours, theirs):
    """(dots that differ, the largest channel error, a picture of ours, theirs and the dots) for our frame
    against the core's, ours centred in it as Beetle Saturn frames a 320x224 screen in 330x240."""
    dx, dy = (theirs.width - ours.width) // 2, (theirs.height - ours.height) // 2
    w, h = ours.width, ours.height
    side = bytearray(w * 3 * h * 3)
    n = worst = 0
    for y in range(h):
        a = ours.rgb[y * w * 3:(y + 1) * w * 3]
        o = ((y + dy) * theirs.width + dx) * 3
        b = theirs.rgb[o:o + w * 3]
        row = bytearray(a) + bytearray(b) + bytearray(c // 3 for c in a)
        if a != b:
            for x in range(0, w * 3, 3):
                e = max(abs(a[x] - b[x]), abs(a[x + 1] - b[x + 1]), abs(a[x + 2] - b[x + 2]))
                if e:
                    n += 1
                    worst = max(worst, e)
                    row[2 * w * 3 + x:2 * w * 3 + x + 3] = b"\xff\x00\xff"
        side[y * w * 9:(y + 1) * w * 9] = row
    return n, worst, agent.Frame(w * 3, h, bytes(side))


def writes(spec):
    """{VBlank: [(addr, bytes)]} from --write's "VBLANK:ADDR=HEX,..."."""
    out = {}
    for item in filter(None, spec):
        at, _, what = item.partition(":")
        addr, _, value = what.partition("=")
        out.setdefault(int(at), []).append((int(addr, 16), bytes.fromhex(value)))
    return out


def presses(spec):
    """{frame: [(port, buttons)]} from --input's "FRAME:[P.]BUTTONS,..."."""
    out = {}
    for item in filter(None, spec.split(",")):
        at, _, what = item.partition(":")
        port, dot, buttons = what.partition(".") if "." in what else ("1", "", what)
        out.setdefault(int(at), []).append((int(port) - 1, set(filter(None, buttons.split("+")))))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("core")
    ap.add_argument("--bios", required=True, help="the folder holding the BIOS")
    ap.add_argument("--cue", required=True)
    ap.add_argument("--frames", type=int, required=True)
    ap.add_argument("--shot", default="", help="frames to save as shot-N.png")
    ap.add_argument("--input", default="")
    ap.add_argument("--pads", type=int, default=1, help="pads plugged in, 1 or 2")
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    os.makedirs(a.out, exist_ok=True)
    core = Core(a.core, a.bios, a.cue, save_dir=a.out, ports=a.pads)
    print(f"{core.av.timing.fps:.4f} frames a second, sound at {core.av.timing.sample_rate:.0f} Hz")
    shots = {int(s) for s in a.shot.split(",") if s}
    due = presses(a.input)
    for n in range(1, a.frames + 1):
        for port, buttons in due.get(n, []):
            core.pads[port] = buttons
        core.run()
        if n in shots and core.frame:
            core.frame.save_png(f"{a.out}/shot-{n}.png")
    core.close()
    print(f"{a.frames} frames run")


if __name__ == "__main__":
    main()
