"""The playtest Worker on this machine under wrangler dev, for a test that needs rooms. Each one starts on an
empty database of its own, and the test invites the testers it needs."""
import contextlib
import json
import os
import shutil
import signal
import socket
import subprocess
import time
import urllib.error
import urllib.request
from collections.abc import Iterator
from dataclasses import dataclass

ADMIN_TOKEN = "local-admin"
WORKER = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))),
                      "playtest", "worker")
WRANGLER = ["pnpm", "exec", "wrangler"]


@dataclass(frozen=True)
class LocalWorker:
    port: int

    def invite(self, name: str) -> str:
        """A new tester's token."""
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}/api/admin/testers", method="POST",
                                         data=json.dumps({"name": name}).encode(),
                                         headers={"authorization": f"Bearer {ADMIN_TOKEN}"})
        with urllib.request.urlopen(request, timeout=10) as r:
            return json.load(r)["token"]

    def room(self, code: str | None = None) -> str:
        """The URL that makes a room, or that joins the room `code`."""
        return f"ws://127.0.0.1:{self.port}/api/rooms/{code or 'new'}"


@contextlib.contextmanager
def local_worker(state: str, log: str, timeout: float = 60) -> Iterator[LocalWorker]:
    """The Worker on a free port, keeping its database in `state` and its output in `log`, until the block ends."""
    shutil.rmtree(state, ignore_errors=True)
    os.makedirs(state)
    subprocess.run([*WRANGLER, "d1", "migrations", "apply", "saturn-playtest", "--local", "--persist-to", state],
                   cwd=WORKER, check=True, capture_output=True, stdin=subprocess.DEVNULL, timeout=300)
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    with open(log, "w") as out:
        # Its own process group, so the end takes workerd with it.
        dev = subprocess.Popen([*WRANGLER, "dev", "--port", str(port), "--ip", "127.0.0.1", "--persist-to", state,
                                "--var", f"ADMIN_TOKEN:{ADMIN_TOKEN}", "--var", "CI_TOKEN:local-ci"],
                               cwd=WORKER, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                               start_new_session=True)
    try:
        _wait_until_up(dev, port, timeout, log)
        yield LocalWorker(port)
    finally:
        os.killpg(dev.pid, signal.SIGTERM)
        try:
            dev.wait(10)
        except subprocess.TimeoutExpired:
            os.killpg(dev.pid, signal.SIGKILL)
            dev.wait()


def _wait_until_up(dev: subprocess.Popen, port: int, timeout: float, log: str) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if dev.poll() is not None:
            raise RuntimeError(f"wrangler dev exited with status {dev.returncode}; see {log}")
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/api/rooms/new", timeout=2)
            return
        except urllib.error.HTTPError:
            # A room answers a plain request with 426, which is the Worker up.
            return
        except OSError:
            time.sleep(0.25)
    raise RuntimeError(f"wrangler dev did not answer on port {port} within {timeout:.0f} s; see {log}")
