"""A session sent to the playtest Worker: the whole session so far as one zip, which replaces the last."""
import io
import json
import os
import re
import urllib.error
import urllib.request
import zipfile

FILES = ["session.json", "input.txt", "clock.txt", "backup-at-start.bin", "state-at-start.bin", "log.txt", "coverage.txt", "progress.txt"]
UNSENT = "unsent"                       # in a session that ended before its last upload got through
USER_AGENT = "saturn-playtest/0.1"      # Cloudflare's edge refuses Python's default user agent (error 1010)


def bundle(session):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name in FILES:
            path = os.path.join(session, name)
            if os.path.exists(path):
                z.write(path, name)
    return out.getvalue()


def vblanks(session):
    """How far the session got: the later of its last recorded press and its log's last timestamp."""
    last = 0
    try:
        presses = open(os.path.join(session, "input.txt")).read().strip(",\n")
        if presses:
            last = int(presses.rsplit(",", 1)[-1].split(":")[0])
    except OSError:
        pass
    try:
        times = re.findall(r"^\[\s*([\d.]+) [A-Z]+\]", open(os.path.join(session, "log.txt"), errors="replace").read(), re.M)
        if times:
            last = max(last, int(float(times[-1]) * 60))
    except OSError:
        pass
    return last


def send(endpoint, token, session, ended):
    """True once the Worker has the session; with `ended` it is then ready for review."""
    info = json.load(open(os.path.join(session, "session.json")))
    body = bundle(session)
    headers = {
        "authorization": f"Bearer {token}",
        "content-type": "application/zip",
        "user-agent": USER_AGENT,
        "x-playtest-product": info["product"],
        "x-playtest-build": info["build"],
        "x-playtest-started": info["started"],
        "x-playtest-vblanks": str(vblanks(session)),
    }
    if ended:
        headers["x-playtest-ended"] = "1"
        headers["x-playtest-exit"] = info.get("exit", "")
    request = urllib.request.Request(f"{endpoint}/api/sessions/{info['id']}", body, headers, method="PUT")
    try:
        with urllib.request.urlopen(request, timeout=60):
            pass
    except urllib.error.HTTPError as e:
        if e.code == 409:                    # reviewed already, or not this tester's: nothing to retry
            return True
        return False
    except OSError:
        return False
    return True


def send_unsent(endpoint, token, sessions_dir):
    """Sends again every session whose last upload failed; the names of those still unsent."""
    left = []
    for name in sorted(os.listdir(sessions_dir)) if os.path.isdir(sessions_dir) else []:
        session = os.path.join(sessions_dir, name)
        marker = os.path.join(session, UNSENT)
        if not os.path.exists(marker):
            continue
        if send(endpoint, token, session, ended=True):
            os.remove(marker)
        else:
            left.append(name)
    return left
