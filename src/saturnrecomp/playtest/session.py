"""One play session: the game in a window, recorded so it can be replayed, and sent as it goes.

A session keeps the pad as the game read it, the clock and saves it started with, the log and the
coverage. Those are enough to replay it on the same build. The runtime rewrites the coverage every
CHECKPOINT seconds, and the session is sent a little after each rewrite and once more at the end.
"""
import datetime
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import threading
import uuid

from . import upload
from .paths import game_dir

CHECKPOINT = 600
SEND_AFTER_CHECKPOINT = 15              # seconds for the runtime to finish writing the coverage


def exit_reason(log_path, code):
    """The first fatal error in the log, or how the runtime exited."""
    try:
        fatal = re.search(r"FATAL\] (.*)", open(log_path, errors="replace").read())
    except OSError:
        fatal = None
    if fatal:
        return fatal.group(1).strip()[:200]
    if code < 0:
        return f"killed by {signal.Signals(-code).name}"
    return "quit" if code == 0 else f"exit {code}"


def play(saturn, cue, info, endpoint=None, token=None, args=(), on_sent=None):
    """Plays a session to its end and sends it; its directory.

    `info` gives the product and the build. Without a token the session is kept but not sent.
    `on_sent(ok, ended)` is called from another thread after each upload.
    """
    started = datetime.datetime.now().replace(microsecond=0)
    sessions = os.path.join(game_dir(info["product"]), "sessions")
    out = os.path.join(sessions, f"{started:%Y%m%d-%H%M%S}-{uuid.uuid4().hex[:8]}")
    os.makedirs(out)
    clock = started.isoformat()
    open(os.path.join(out, "clock.txt"), "w").write(clock + "\n")
    save = os.path.join(game_dir(info["product"]), "backup.bin")
    if os.path.exists(save):
        shutil.copy(save, os.path.join(out, "backup-at-start.bin"))
    record = {"id": os.path.basename(out), "product": info["product"], "build": info["build"],
              "started": started.astimezone(datetime.timezone.utc).isoformat(), "clock": clock,
              "args": list(args), "platform": f"{platform.system()} {platform.machine()}"}
    _write(out, record)

    command = [saturn, "--cue", cue, "--out", out, "--clock", clock,
               "--record-input", os.path.join(out, "input.txt"),
               "--coverage", os.path.join(out, "coverage.txt"), "--checkpoint", str(CHECKPOINT), *args]
    log_path = os.path.join(out, "log.txt")
    with open(log_path, "w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
    stop = threading.Event()
    sender = None
    if token:
        sender = threading.Thread(target=_send_while_playing, args=(endpoint, token, out, stop, on_sent), daemon=True)
        sender.start()
    code = process.wait()
    stop.set()
    if sender:
        sender.join()

    record["exit"] = exit_reason(log_path, code)
    record["ended"] = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()
    _write(out, record)
    with open(log_path, "a") as log:
        log.write(f"exit: {signal.Signals(-code).name if code < 0 else code}\n")
    if token:
        ok = upload.send(endpoint, token, out, ended=True)
        if not ok:
            open(os.path.join(out, upload.UNSENT), "w").close()
        if on_sent:
            on_sent(ok, True)
    return out


def _send_while_playing(endpoint, token, out, stop, on_sent):
    """Sends the session once at the start, so it shows on the tester's page at once, then after each checkpoint."""
    wait = SEND_AFTER_CHECKPOINT
    while not stop.wait(wait):
        ok = upload.send(endpoint, token, out, ended=False)
        if on_sent:
            on_sent(ok, False)
        wait = CHECKPOINT


def _write(out, record):
    with open(os.path.join(out, "session.json"), "w") as f:
        json.dump(record, f, indent=1)
