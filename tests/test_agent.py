"""The agent client against a stand-in for the saturn executable that speaks the same protocol:
memory is 16 bytes counting up, a frame is 2x1 red then blue, and a step runs that many VBlanks."""
import os
import stat
import sys
import textwrap

import pytest

from saturnrecomp import agent

FAKE = textwrap.dedent(f"""\
    #!{sys.executable}
    import socket, sys
    fd = int(sys.argv[sys.argv.index("--agent") + 1])
    s = socket.socket(fileno=fd)
    f = s.makefile("rb")
    vblank = 1
    s.sendall(b"vblank 1\\n")
    for line in f:
        cmd, *args = line.decode().split()
        if cmd == "step":
            vblank += int(args[0])
            if vblank > 100:
                sys.exit(0)
            s.sendall(f"vblank {{vblank}}\\n".encode())
        elif cmd == "pad":
            s.sendall(b"error no button Q\\n" if args and "Q" in args[0] else b"ok\\n")
        elif cmd == "read":
            a, n = int(args[0], 16), int(args[1])
            s.sendall(("data " + bytes((a + i) & 0xFF for i in range(n)).hex()).encode() + b"\\n")
        elif cmd == "frame":
            s.sendall(b"frame 2 1\\n" + bytes([255, 0, 0, 0, 0, 255]))
        elif cmd == "quit":
            break
    """)


@pytest.fixture
def saturn(tmp_path):
    path = tmp_path / "saturn"
    path.write_text(FAKE)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return str(path)


def test_steps_and_keeps_presses_as_input_steps(saturn, tmp_path):
    with agent.start(saturn, [], str(tmp_path / "run")) as run:
        assert run.vblank == 1
        run.pad("START")
        assert run.step(10) == 11
        run.pad("")
        run.pad("A", pad=3)
        assert run.presses == ["1:START", "11:", "11:3.A"]


def test_reads_memory_and_frames(saturn, tmp_path):
    with agent.start(saturn, [], str(tmp_path / "run")) as run:
        assert run.read(0x060D5F00, 3) == bytes([0x00, 0x01, 0x02])
        assert run.read32(0x10) == 0x10111213
        frame = run.frame()
        assert (frame.width, frame.height) == (2, 1)
        assert frame.pixel(1, 0) == (0, 0, 255)
        frame.save_png(str(tmp_path / "f.png"))
        assert open(tmp_path / "f.png", "rb").read(8) == b"\x89PNG\r\n\x1a\n"


def test_a_refused_command_raises_and_is_not_kept(saturn, tmp_path):
    with agent.start(saturn, [], str(tmp_path / "run")) as run:
        with pytest.raises(agent.AgentError, match="no button Q"):
            run.pad("Q")
        with pytest.raises(agent.AgentError):
            run.step(0)
        assert run.presses == []


def test_a_run_that_stops_raises_run_ended(saturn, tmp_path):
    run = agent.start(saturn, [], str(tmp_path / "run"))
    with pytest.raises(agent.RunEnded):
        run.step(200)
    run.close()
    assert os.path.exists(tmp_path / "run" / "log.txt")
