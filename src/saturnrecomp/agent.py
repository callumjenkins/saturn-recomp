"""Play a run from Python through the saturn executable's --agent socket: step VBlanks, press the
pads, read and write memory, take frames.

    with agent.start(game.saturn, ["--cue", CUE, "--headless"], "build/run/bot") as run:
        run.pad("START")
        run.step(10)
        run.pad("")
        frame = run.frame()

The run pauses at VBlank 1. `presses` keeps every press as an --input step, so a run played this
way can be replayed, or kept as a route, without the program that played it."""
import os
import socket
import struct
import subprocess
import zlib
from dataclasses import dataclass


class AgentError(Exception):
    """The run refused a command."""


class RunEnded(Exception):
    """The run stopped: a fatal error, its VBlank limit, or quit."""


@dataclass
class Frame:
    width: int
    height: int
    rgb: bytes                                   # 3 bytes a dot, top row first

    def pixel(self, x, y):
        i = (y * self.width + x) * 3
        return tuple(self.rgb[i:i + 3])

    def save_png(self, path):
        rows = b"".join(b"\0" + self.rgb[y * self.width * 3:(y + 1) * self.width * 3] for y in range(self.height))
        chunk = lambda kind, data: (struct.pack(">I", len(data)) + kind + data
                                    + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.width, self.height, 8, 2, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


class Run:
    def __init__(self, saturn, args, out):
        os.makedirs(out, exist_ok=True)
        self.out = out
        ours, theirs = socket.socketpair()
        self._log = open(os.path.join(out, "log.txt"), "w")
        self._proc = subprocess.Popen([saturn, "--out", out, "--agent", str(theirs.fileno()), *args],
                                      pass_fds=[theirs.fileno()], stdout=self._log, stderr=subprocess.STDOUT)
        theirs.close()
        self._sock = ours
        self._in = ours.makefile("rb")
        self.presses = []
        self.vblank = self._expect_vblank()

    def _line(self):
        line = self._in.readline()
        if not line:
            raise RunEnded(f"the run stopped; its log is {self.out}/log.txt")
        return line.decode().rstrip("\n")

    def _ask(self, command):
        self._sock.sendall(command.encode() + b"\n")
        reply = self._line()
        if reply.startswith("error "):
            raise AgentError(reply[6:])
        return reply

    def _expect_vblank(self):
        reply = self._line()
        if not reply.startswith("vblank "):
            raise AgentError(f"expected vblank, got {reply}")
        return int(reply[7:])

    def step(self, n=1):
        """Run n VBlanks; the VBlank it pauses at."""
        self._sock.sendall(f"step {n}\n".encode())
        self.vblank = self._expect_vblank()
        return self.vblank

    def pad(self, buttons="", pad=1):
        """Pad `pad` holds `buttons` ("A+UP"; "" lets go) from this VBlank on."""
        spec = buttons if pad == 1 else f"{pad}.{buttons}"
        self._ask(f"pad {spec}")
        self.presses.append(f"{self.vblank}:{spec}")

    def read(self, addr, n):
        return bytes.fromhex(self._ask(f"read {addr:X} {n}")[5:])

    def read32(self, addr):
        return int.from_bytes(self.read(addr, 4), "big")

    def write(self, addr, data):
        self._ask(f"write {addr:X} {bytes(data).hex()}")

    def frame(self):
        w, h = map(int, self._ask("frame").split()[1:])
        rgb = self._in.read(w * h * 3)
        if len(rgb) != w * h * 3:
            raise RunEnded("the run stopped during a frame")
        return Frame(w, h, rgb)

    def close(self):
        """End the run; its log."""
        try:
            self._sock.sendall(b"quit\n")
        except OSError:
            pass
        self._sock.close()
        self._proc.wait()
        self._log.close()
        return open(os.path.join(self.out, "log.txt")).read()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def start(saturn, args, out):
    """A run of the saturn executable at `saturn`, paused at VBlank 1. `args` are its own, without
    --out and --agent."""
    return Run(saturn, args, out)
