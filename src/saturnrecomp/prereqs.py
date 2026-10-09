"""What a machine needs to build and play a game: the tools on PATH, a C++ compiler CMake can use,
and SDL3 for the window, the pads and the sound.

    python -m saturnrecomp.prereqs

Each check gives a problem or nothing, with the platform's install command when it knows one.
ffmpeg is optional: only --video and replays recorded as video use it.
"""
import os
import platform
import shutil
import subprocess
import sys
import tempfile

TOOLS = ("cmake", "ninja", "clang++")

# The packages for TOOLS and SDL3, then ffmpeg, by the ID in /etc/os-release (or the platform).
INSTALL = {
    "fedora": ("sudo dnf install cmake ninja-build clang SDL3-devel", "sudo dnf install ffmpeg-free"),
    "debian": ("sudo apt install cmake ninja-build clang libsdl3-dev", "sudo apt install ffmpeg"),
    "ubuntu": ("sudo apt install cmake ninja-build clang libsdl3-dev", "sudo apt install ffmpeg"),
    "arch": ("sudo pacman -S cmake ninja clang sdl3", "sudo pacman -S ffmpeg"),
    "macos": ("xcode-select --install; brew install cmake ninja sdl3", "brew install ffmpeg"),
}

PROBE = """cmake_minimum_required(VERSION 3.20)
project(probe CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(SDL3 CONFIG)
message(STATUS "probe: SDL3=${SDL3_FOUND} ${SDL3_VERSION}")
"""


def system():
    """fedora, debian, ubuntu, arch, macos, windows, or the os-release ID as it is."""
    if sys.platform == "darwin":
        return "macos"
    if sys.platform == "win32":
        return "windows"
    try:
        fields = dict(line.rstrip().split("=", 1) for line in open("/etc/os-release") if "=" in line)
    except OSError:
        return platform.system().lower()
    ids = [fields.get("ID", "").strip('"')] + fields.get("ID_LIKE", "").strip('"').split()
    return next((i for i in ids if i in INSTALL), ids[0])


def probe(cxx="clang++"):
    """(problem or None, SDL3 version or None): CMake configures a C++20 project with the compiler
    and looks for SDL3 as the runtime's build does."""
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "CMakeLists.txt"), "w").write(PROBE)
        r = subprocess.run(["cmake", "-S", d, "-B", os.path.join(d, "b"), "-G", "Ninja", f"-DCMAKE_CXX_COMPILER={cxx}"],
                           capture_output=True, text=True)
    if r.returncode:
        tail = "\n    ".join((r.stdout + r.stderr).strip().splitlines()[-8:])
        return f"CMake cannot build C++ with {cxx}:\n    {tail}", None
    found = next((line.split("probe: ", 1)[1] for line in r.stdout.splitlines() if "probe: " in line), "")
    sdl, _, version = found.removeprefix("SDL3=").partition(" ")
    return None, (version or "found") if sdl.upper() in ("1", "TRUE", "ON", "YES") else None


def check(log=print):
    """Logs each finding; returns the problems that stop a build or a windowed run."""
    where = system()
    needed, optional = INSTALL.get(where, (None, None))
    problems = []
    missing = [t for t in TOOLS if not shutil.which(t)]
    if missing:
        problems.append("missing " + ", ".join(missing))
    else:
        failed, sdl = probe()
        if failed:
            problems.append(failed)
        elif not sdl:
            problems.append("SDL3 not found: the build would have no window, pads or sound (headless runs only)")
        else:
            log(f"ok: {', '.join(TOOLS)}, SDL3 {sdl}")
    if where == "windows":
        problems.append("Windows is not supported yet")
    if problems and needed:
        problems.append(f"to install them on {where}: {needed}")
    if shutil.which("ffmpeg"):
        log("ok: ffmpeg, for --video")
    else:
        log("note: no ffmpeg, so no --video recordings; playing needs none" + (f" ({optional})" if optional else ""))
    for p in problems:
        log("problem: " + p)
    return problems


def main():
    raise SystemExit(1 if check() else 0)


if __name__ == "__main__":
    main()
