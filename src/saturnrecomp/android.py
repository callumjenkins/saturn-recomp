"""saturn on an Android phone over adb, with saturn's own arguments, so a check that takes another executable
(such as tests/frames.py --saturn) runs the phone's build. tools/saturn-android starts it.

The phone needs a debuggable playtest build (-Pplaytest.debuggable), whose launcher plays the arguments it is handed
(LauncherActivity's playFromAdb), and the disc chosen in that launcher, which stands in for --cue. Files the run reads
are pushed to the phone; files it writes, and --out, are pulled back to where the arguments name. adb picks the phone
by ANDROID_SERIAL as usual, and SATURN_ANDROID_PACKAGE names the build when more than one is debuggable. A --session
joins as the tester SATURN_PLAYTEST_TESTER names. Runs on one phone take turns. The exit status is 1 when the log has
a fatal error or the game stopped without saying why.

Uses the standard library alone, so it runs without the project's environment."""
import fcntl
import io
import json
import os
import posixpath
import shlex
import subprocess
import sys
import tarfile
import tempfile
import time
from collections.abc import Callable
from dataclasses import dataclass, field

ACTIVITY = "org.saturnrecomp.playtest.LauncherActivity"
ADB_RUN = "adb-run"   # under the app's files
OUTPUTS = ("--state-out", "--wav", "--record-input", "--coverage", "--progress", "--after")
PACKAGE_PREFIX = "org.saturnrecomp.playtest."
STAGING = "/data/local/tmp/saturn-android-file"
UNSUPPORTED = ("--agent", "--video", "--other-disc")


@dataclass
class Plan:
    """A run's arguments as the phone takes them, with what goes there first and what comes back after."""
    args: list[str]
    pushes: list[tuple[str, str]] = field(default_factory=list)   # (host file, phone path)
    pulls: list[tuple[str, str]] = field(default_factory=list)    # (phone path, host file)
    work: str = ""
    out: str | None = None


class _turn:
    """Holds the phone for one run, so runs started side by side take turns."""

    def __enter__(self) -> None:
        serial = os.environ.get("ANDROID_SERIAL", "default")
        self.file = open(os.path.join(tempfile.gettempdir(), f"saturn-android-{serial}.lock"), "w")
        fcntl.flock(self.file, fcntl.LOCK_EX)

    def __exit__(self, *_: object) -> None:
        self.file.close()


def main(argv: list[str]) -> int:
    package = _package()
    plan = plan_run(argv, _cue(package), f"/data/user/0/{package}/files/{ADB_RUN}", os.path.isfile)
    with _turn():
        return _run(package, plan)


def plan_run(argv: list[str], cue: str, work: str, exists: Callable[[str], bool]) -> Plan:
    """`argv` for the phone: --cue becomes `cue`, --out and every file written lands in the folder `work`, and every
    file read that `exists` here is pushed there. The game's own folder is /, so `work` is absolute. Raises ValueError for an argument the phone cannot take."""
    plan = Plan(args=[], work=work)
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in UNSUPPORTED:
            raise ValueError(f"{a} is not available over adb")
        if "\n" in a:
            raise ValueError(f"an argument holds a newline: {a!r}")
        value = argv[i + 1] if i + 1 < len(argv) else None
        if a == "--cue" and value is not None:
            plan.args += [a, cue]
            i += 2
            continue
        if a == "--out" and value is not None:
            plan.out = value
            plan.args += [a, f"{plan.work}/out"]
            i += 2
            continue
        if a == "--log":
            raise ValueError("--log is the wrapper's: saturn's log comes back on stdout")
        if a in OUTPUTS and value is not None:
            phone = f"{plan.work}/written/{len(plan.pulls)}-{posixpath.basename(value)}"
            plan.pulls.append((phone, value))
            plan.args += [a, phone]
            i += 2
            continue
        if a == "--save" and value is not None and value != "-":
            phone = f"{plan.work}/save/{posixpath.basename(value)}"
            if exists(value):
                plan.pushes.append((value, phone))
            plan.pulls.append((phone, value))
            plan.args += [a, phone]
            i += 2
            continue
        plan.args.append(_pushed(plan, a, exists))
        i += 1
    plan.args += ["--log", f"{plan.work}/log.txt"]
    return plan


def _adb(*args: str, check: bool = True) -> bytes:
    return subprocess.run(["adb", *args], capture_output=True, check=check, timeout=600).stdout


def _cue(package: str) -> str:
    """The disc the phone's launcher has: SATURN_ANDROID_CUE, or the one launcher.json names."""
    if cue := os.environ.get("SATURN_ANDROID_CUE"):
        return cue
    found = _run_as(package, "ls files/saturn-recomp/*/launcher.json", check=False).decode().split()
    for path in found:
        if cue := json.loads(_run_as(package, f"cat {shlex.quote(path)}")).get("cue"):
            return cue
    raise SystemExit(f"{package} has no disc: choose one in its launcher, or set SATURN_ANDROID_CUE")


def _game_running(package: str) -> bool:
    return bool(_adb("shell", "pidof", f"{package}:game", check=False).strip())


def _package() -> str:
    """SATURN_ANDROID_PACKAGE, or the one playtest build on the phone that adb can reach the files of."""
    if package := os.environ.get("SATURN_ANDROID_PACKAGE"):
        return package
    listed = _adb("shell", "pm", "list", "packages", PACKAGE_PREFIX).decode().split()
    names = [p.removeprefix("package:") for p in listed]
    debuggable = [n for n in names if subprocess.run(["adb", "shell", "run-as", n, "true"], capture_output=True).returncode == 0]
    if len(debuggable) != 1:
        raise SystemExit(f"set SATURN_ANDROID_PACKAGE: the debuggable playtest builds on the phone are {debuggable or 'none'}")
    return debuggable[0]


def _pushed(plan: Plan, arg: str, exists: Callable[[str], bool]) -> str:
    """`arg`, or its phone path when it names a file here: a bare path, or --input's @FILE."""
    at = arg.startswith("@")
    path = arg[1:] if at else arg
    if not path or path.startswith("-") or not exists(path):
        return arg
    phone = f"{plan.work}/read/{len(plan.pushes)}-{posixpath.basename(path)}"
    plan.pushes.append((path, phone))
    return ("@" if at else "") + phone


def _run(package: str, plan: Plan) -> int:
    _adb("shell", "am", "force-stop", package)
    _adb("shell", "input", "keyevent", "KEYCODE_WAKEUP")
    if b"isKeyguardShowing=true" in _adb("shell", "dumpsys", "window"):
        raise SystemExit("saturn-android: unlock the phone; a game behind the lock screen is paused")
    _run_as(package, f"rm -rf {plan.work} && mkdir -p {plan.work}/out {plan.work}/written {plan.work}/read {plan.work}/save")
    with tempfile.TemporaryDirectory() as here:
        listed = os.path.join(here, "args.txt")
        with open(listed, "w", encoding="utf-8") as f:
            f.writelines(f"{a}\n" for a in plan.args)
        for host, phone in [*plan.pushes, (listed, f"{plan.work}/args.txt")]:
            _put(package, host, phone)
    tester = os.environ.get("SATURN_PLAYTEST_TESTER")
    started = _adb("shell", "am", "start", "-n", f"{package}/{ACTIVITY}", "--es", "args", f"{ADB_RUN}/args.txt",
                   *(["--es", "tester", shlex.quote(tester)] if tester else []), check=False).decode()
    if "Error" in started:
        raise SystemExit(f"saturn-android: the phone would not start the game: {started.strip()}")
    began = time.monotonic()
    while not _game_running(package):
        if time.monotonic() - began > 30:
            raise SystemExit(f"{package}'s game did not start")
        time.sleep(0.2)
    while _game_running(package):
        time.sleep(0.5)
    log = _run_as(package, f"cat {plan.work}/log.txt", check=False).decode(errors="replace")
    sys.stdout.write(log)
    if plan.out:
        os.makedirs(plan.out, exist_ok=True)
        with tarfile.open(fileobj=io.BytesIO(_run_as(package, f"tar -C {plan.work}/out -cf - ."))) as tar:
            tar.extractall(plan.out, filter="data")
    for phone, host in plan.pulls:
        if written := _run_as(package, f"cat {shlex.quote(phone)} 2>/dev/null", check=False):
            with open(host, "wb") as f:
                f.write(written)
    stopped = "stopped (" in log
    return 1 if "FATAL" in log or not stopped else 0


def _put(package: str, host: str, phone: str) -> None:
    """Copies `host` to `phone`, under `package`'s data folder, through the shell's own folder, which the app can read."""
    _adb("push", host, STAGING)
    _run_as(package, f"cp {STAGING} {shlex.quote(phone)}")
    _adb("shell", "rm", STAGING)


def _run_as(package: str, command: str, check: bool = True) -> bytes:
    """`command`, a shell line, run as `package` in its data folder. adb hands the phone's shell one line, so it is
    quoted whole."""
    return _adb("exec-out", f"run-as {package} sh -c {shlex.quote(command)}", check=check)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except ValueError as e:
        sys.exit(f"saturn-android: {e}")
