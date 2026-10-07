"""The maintainer's side of playtesting, for a game whose game.toml has a [playtest] table.

    python -m saturnrecomp.playtest GAME.toml register          the game, so its builds and sessions are accepted
    python -m saturnrecomp.playtest GAME.toml invite NAME       a tester code and the link to their sessions page
    python -m saturnrecomp.playtest GAME.toml revoke TESTER_ID
    python -m saturnrecomp.playtest GAME.toml publish           the generated C++ to the Worker, then the release workflow
    python -m saturnrecomp.playtest GAME.toml pull              sessions ready for review, into BUILD/playtest/ID
    python -m saturnrecomp.playtest GAME.toml review ID [--add-seeds]
    python -m saturnrecomp.playtest GAME.toml reviewed ID SUMMARY|@FILE
    python -m saturnrecomp.playtest GAME.toml replay ID [--video]
    python -m saturnrecomp.playtest GAME.toml build-id          the id the current build would be published as
    python -m saturnrecomp.playtest GAME.toml product           the disc's product, as the Worker and the saves name it
    python -m saturnrecomp.playtest GAME.toml bundle BUILD DIR  playtest.json and disc.json for a release's folder

SATURN_PLAYTEST_ADMIN_TOKEN holds the Worker's ADMIN_TOKEN. `publish` runs the release workflow
through the `gh` command, which needs to be signed in to the game's repository.
"""
import argparse
import datetime
import glob
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.error
import urllib.request
import zipfile

from .. import build, config
from . import review
from .upload import USER_AGENT, vblanks

RUNTIME = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))), "runtime")
WORKFLOW = "playtest.yml"


def api(game, method, path, body=None, raw=False):
    """The Worker's answer as JSON, or as bytes with `raw`; a failure ends the command with the Worker's message."""
    token = os.environ.get("SATURN_PLAYTEST_ADMIN_TOKEN")
    if not token:
        raise SystemExit("set SATURN_PLAYTEST_ADMIN_TOKEN to the Worker's ADMIN_TOKEN")
    data = body if isinstance(body, bytes) or body is None else json.dumps(body).encode()
    request = urllib.request.Request(game.playtest.endpoint + path, data, {"authorization": f"Bearer {token}", "user-agent": USER_AGENT}, method=method)
    try:
        with urllib.request.urlopen(request, timeout=300) as r:
            out = r.read()
    except urllib.error.HTTPError as e:
        raise SystemExit(f"{method} {path}: {e.code} {e.read().decode(errors='replace')}")
    return out if raw else json.loads(out)


def bundle(game, ident, out):
    """What the launcher reads beside it: the game, the build, where to send sessions, and the disc it plays."""
    os.makedirs(out, exist_ok=True)
    info = {"name": game.name, "product": game.playtest.product, "build": ident, "endpoint": game.playtest.endpoint,
            "repo": game.playtest.repo, "args": game.playtest.args, "boxart": game.playtest.boxart}
    json.dump(info, open(os.path.join(out, "playtest.json"), "w"), indent=1)
    shutil.copy(game.playtest.disc, os.path.join(out, "disc.json"))


def build_id(game):
    """The date and a hash of what the build is made from: the recompile's inputs and the runtime's source."""
    h = hashlib.sha1(open(os.path.join(game.build, "recomp", "inputs.json"), "rb").read())
    for path in sorted(glob.glob(os.path.join(RUNTIME, "**", "*"), recursive=True)):
        if os.path.isfile(path):
            h.update(os.path.relpath(path, RUNTIME).encode() + b"\0" + open(path, "rb").read())
    return f"{datetime.date.today():%Y%m%d}-{h.hexdigest()[:8]}"


def publish(game):
    build.ensure(game)
    ident = build_id(game)
    sources = io.BytesIO()
    with tarfile.open(fileobj=sources, mode="w:gz") as tar:
        tar.add(os.path.join(game.build, "recomp"), arcname="recomp")
    product = game.playtest.product
    api(game, "PUT", f"/api/admin/sources/{product}/{ident}", sources.getvalue())
    print(f"build {ident}: {len(sources.getvalue()) >> 20} MB of generated C++ sent")
    command = ["gh", "workflow", "run", WORKFLOW, "-R", game.playtest.repo, "-f", f"build={ident}"]
    if shutil.which("gh"):
        subprocess.run(command, check=True)
        print(f"release workflow started: gh run list -R {game.playtest.repo} -w {WORKFLOW}")
    else:
        print("start the release with: " + " ".join(command))


def pull(game, pulled):
    sessions = api(game, "GET", f"/api/admin/sessions?status=pending&product={game.playtest.product}")["sessions"]
    for s in sessions:
        out = os.path.join(pulled, s["id"])
        os.makedirs(out, exist_ok=True)
        with zipfile.ZipFile(io.BytesIO(api(game, "GET", f"/api/admin/sessions/{s['id']}/bundle", raw=True))) as z:
            z.extractall(out)
        json.dump(s, open(os.path.join(out, "server.json"), "w"), indent=1)
        minutes = s["vblanks"] // 3600
        print(f"{s['id']}  {s['tester']:12} build {s['build']}  {minutes:4} min  {s['exit'] or s['status']}")
    if not sessions:
        print("no sessions waiting for review")


def replay(game, session, video):
    """The session played again headless on the current build, into its replay/ directory."""
    info = json.load(open(os.path.join(session, "session.json")))
    out = os.path.join(session, "replay")
    os.makedirs(out, exist_ok=True)
    save = "-"
    if os.path.exists(os.path.join(session, "backup-at-start.bin")):
        save = shutil.copy(os.path.join(session, "backup-at-start.bin"), os.path.join(out, "backup.bin"))
    recorded = os.path.join(session, "input.txt")
    has_presses = os.path.exists(recorded) and open(recorded).read().strip(",\n")
    args = [game.saturn, "--cue", _cue(), "--out", out, "--save", save, "--clock", info["clock"], "--headless",
            *(["--input", f"@{recorded}"] if has_presses else []), "--vblanks", str(vblanks(session) + 600),
            "--coverage", os.path.join(out, "coverage.txt"), *info.get("args", []),
            *(["--video", os.path.abspath(os.path.join(out, "video.mp4"))] if video else [])]
    with open(os.path.join(out, "log.txt"), "w") as log:
        code = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT).returncode
    print(f"replayed into {out}, code {code}")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python -m saturnrecomp.playtest", description=__doc__.split("\n")[0])
    ap.add_argument("config")
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("register")
    sub.add_parser("invite").add_argument("name")
    sub.add_parser("revoke").add_argument("tester")
    sub.add_parser("publish")
    sub.add_parser("pull")
    r = sub.add_parser("review")
    r.add_argument("session")
    r.add_argument("--add-seeds", action="store_true", help="add what discovery missed to the game's seeds")
    r = sub.add_parser("reviewed")
    r.add_argument("session")
    r.add_argument("summary", help="the text testers see, or @FILE holding it")
    r = sub.add_parser("replay")
    r.add_argument("session")
    r.add_argument("--video", action="store_true")
    sub.add_parser("product")
    sub.add_parser("build-id")
    r = sub.add_parser("bundle")
    r.add_argument("build")
    r.add_argument("out")
    a = ap.parse_args(argv)
    game = config.load(a.config)
    if not game.playtest:
        raise SystemExit(f"{a.config} has no [playtest] table")
    pulled = os.path.join(game.build, "playtest")
    session = os.path.join(pulled, getattr(a, "session", "") or "")

    if a.command == "register":
        print(api(game, "PUT", f"/api/admin/games/{game.playtest.product}", {"name": game.name, "repo": game.playtest.repo}))
    elif a.command == "invite":
        t = api(game, "POST", "/api/admin/testers", {"name": a.name})
        print(f"tester {t['name']} ({t['id']})\ncode: {t['token']}\nsessions page: {game.playtest.endpoint}/#token={t['token']}")
    elif a.command == "revoke":
        print(api(game, "DELETE", f"/api/admin/testers/{a.tester}"))
    elif a.command == "publish":
        publish(game)
    elif a.command == "pull":
        pull(game, pulled)
    elif a.command == "review":
        found = review.report(game, session, pulled)
        json.dump(found, open(os.path.join(session, "review.json"), "w"), indent=1)
        print(json.dumps(found, indent=1))
        if a.add_seeds and found["missed"]:
            seeds = game.learned_seeds()
            for entry in found["missed"]:
                module, addr = entry.split()
                if module != "?" and int(addr, 16) not in seeds.get(module, []):
                    seeds.setdefault(module, []).append(int(addr, 16))
            game.save_learned_seeds(seeds)
            print(f"seeds added to {os.path.relpath(game.seeds_file, game.root)}: the next build recompiles")
    elif a.command == "reviewed":
        found = json.load(open(os.path.join(session, "review.json")))
        summary = open(a.summary[1:]).read().strip() if a.summary.startswith("@") else a.summary
        body = {"summary": summary, "new_functions": len(found["missed"]), "new_bytes": found["new_bytes"], "missed": found["missed"]}
        print(api(game, "POST", f"/api/admin/sessions/{a.session}/review", body))
        json.dump(body, open(os.path.join(session, review.REVIEWED), "w"), indent=1)
    elif a.command == "replay":
        replay(game, session, a.video)
    elif a.command == "product":
        print(game.playtest.product)
    elif a.command == "build-id":
        print(build_id(game))
    elif a.command == "bundle":
        bundle(game, a.build, a.out)


def _cue():
    """The maintainer's own disc: SATURN_CUE, or the one .cue in iso/."""
    if os.environ.get("SATURN_CUE"):
        return os.environ["SATURN_CUE"]
    found = glob.glob("iso/*.cue")
    if len(found) != 1:
        raise SystemExit("set SATURN_CUE to the disc's .cue")
    return found[0]


if __name__ == "__main__":
    main(sys.argv[1:])
