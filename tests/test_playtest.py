import http.server
import io
import json
import os
import stat
import sys
import threading
import zipfile

import pytest

from saturnrecomp.playtest import coverage, disc_image, review, session, upload

FAKE_SATURN = """#!{python}
import sys, time
args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
open(args["--record-input"], "w").write("120:START,130:,")
time.sleep(0.5)
open(args["--coverage"], "w").write("KRNL 06010000 4 1\\nKRNL 06010010 2 0\\n")
print("[  10.000 M] pad: FFF7")
print("[  12.500 FATAL] call to 06012345, not an entry of an active module (from pr 06010004)")
sys.exit(1)
"""


class Receiver(http.server.BaseHTTPRequestHandler):
    uploads = []

    def do_PUT(self):
        body = self.rfile.read(int(self.headers["content-length"]))
        Receiver.uploads.append((self.path, dict(self.headers), body))
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"{}")

    def log_message(self, *args):
        pass


@pytest.fixture
def endpoint():
    Receiver.uploads = []
    server = http.server.HTTPServer(("127.0.0.1", 0), Receiver)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{server.server_port}"
    server.shutdown()


def test_a_session_is_sent_while_playing_and_once_more_when_it_ends(tmp_path, monkeypatch, endpoint):
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "data"))
    monkeypatch.setattr(session, "SEND_AFTER_CHECKPOINT", 0.1)
    saturn = tmp_path / "saturn"
    saturn.write_text(FAKE_SATURN.format(python=sys.executable))
    saturn.chmod(saturn.stat().st_mode | stat.S_IEXEC)

    out = session.play(str(saturn), "disc.cue", {"product": "MK-81070_V1.003", "build": "20261007-abcdef12"}, endpoint, "code")

    assert os.path.dirname(out) == str(tmp_path / "data" / "saturn-recomp" / "MK-81070_V1.003" / "sessions")
    first, last = Receiver.uploads[0], Receiver.uploads[-1]
    assert first[0] == f"/api/sessions/{os.path.basename(out)}" and "X-Playtest-Ended" not in first[1]
    assert last[1]["X-Playtest-Ended"] == "1"
    assert last[1]["User-Agent"] == upload.USER_AGENT
    assert last[1]["X-Playtest-Exit"].startswith("call to 06012345")
    assert last[1]["X-Playtest-Vblanks"] == "750"
    with zipfile.ZipFile(io.BytesIO(last[2])) as z:
        assert {"session.json", "input.txt", "clock.txt", "log.txt", "coverage.txt"} <= set(z.namelist())
        assert json.loads(z.read("session.json"))["build"] == "20261007-abcdef12"


def test_a_session_whose_last_upload_fails_is_sent_next_time(tmp_path, endpoint):
    s = tmp_path / "sessions" / "20261007-100000-aaaaaaaa"
    s.mkdir(parents=True)
    (s / "session.json").write_text(json.dumps({"id": s.name, "product": "P_V1", "build": "b", "started": "2026-10-07T10:00:00+00:00", "exit": "quit"}))
    (s / upload.UNSENT).touch()

    assert upload.send_unsent("http://127.0.0.1:9", "code", str(tmp_path / "sessions")) == [s.name]
    assert upload.send_unsent(endpoint, "code", str(tmp_path / "sessions")) == []
    assert not (s / upload.UNSENT).exists()


@pytest.mark.parametrize("name, head, mode", [
    ("raw", b"\x00" + b"\xff" * 10 + b"\x00", "MODE1/2352"),
    ("user data", b"SEGA SEGASATURN ", "MODE1/2048"),
])
def test_a_lone_image_gets_a_cue_naming_it_by_absolute_path(tmp_path, name, head, mode):
    image = tmp_path / "game.iso"
    image.write_bytes(head + bytes(64))
    cue = open(disc_image.cue_for(str(image), str(tmp_path / "home"))).read()
    assert f'FILE "{image}" BINARY' in cue and f"TRACK 01 {mode}" in cue


def test_new_code_is_what_no_known_run_reached(tmp_path):
    known = tmp_path / "known.txt"
    known.write_text("KRNL 06010000 4 1\nKRNL 06010010 2 0\n")
    mine = tmp_path / "mine.txt"
    mine.write_text("KRNL 06010000 4 1\nKRNL 06010010 2 1\n")
    assert len(review.instructions(str(mine)) - review.instructions(str(known))) == 2


def test_the_coverage_map_sorts_unreached_code_by_what_leads_to_it(tmp_path):
    def f(addr, body):
        return f"void f_{addr:08X}(SH2Context& c) {{\n" + "".join(f"    {line}\n" for line in body) + "}\n"
    ins = lambda a: f"c.r[0] = 0;  // {a:08X} nop"
    (tmp_path / "p_krnl_000.cpp").write_text(
        f(0x06000000, [ins(0x06000000), "f_06000100(c);  // 06000002 bsr", ins(0x06000004)])     # reached
        + f(0x06000100, [ins(0x06000100), "f_06000200(c);  // 06000102 bsr"])                    # behind it
        + f(0x06000200, [ins(0x06000200)])                                                       # under that
        + f(0x06000300, [ins(0x06000300), ins(0x06000302)])                                      # in a table
        + f(0x06000400, [ins(0x06000400)])                                                       # nothing leads here
        + f(0x06000004, [ins(0x06000004)]))                                                      # inside the reached one
    cov = tmp_path / "coverage.txt"
    cov.write_text("KRNL 06000000 3 1\nKRNL 06000100 2 0\n")
    insns, calls = coverage.generated(str(tmp_path), ["KRNL"])
    image = bytes.fromhex("06000300")
    found = coverage.build(insns, calls, coverage.entered([str(cov)], insns), coverage.pointed_at([image], insns))
    assert found["modules"]["KRNL"] == {"code": 18, "reached": 6, "behind": 6, "pointer": 4, "orphan": 2}
    assert [(n, kind, f"{r[1]:08X}", [f"{c[1]:08X}" for c in cs]) for n, kind, r, cs in found["roots"]] == [
        (6, "behind", "06000100", ["06000000"]), (4, "pointer", "06000300", []), (2, "orphan", "06000400", [])]


def test_the_report_sets_classed_groups_aside_and_lists_them_on_request(tmp_path):
    classes = tmp_path / "classes.tsv"
    classes.write_text("# dead code\nKRNL 06000400 dead  no release calls it\n")
    found = {"modules": {"KRNL": {"code": 18, "reached": 6, "behind": 6, "orphan": 4}},
             "roots": [(6, "behind", ("KRNL", 0x06000100), []), (4, "orphan", ("KRNL", 0x06000400), [])],
             "classes": coverage.classes(str(classes))}
    text = coverage.report(found, {})
    assert "dead                1 groups        0 behind        0 pointer        4 orphan" in text
    assert "06000400" not in text.split("by their root:")[1]
    assert "06000400  [dead]" in coverage.report(found, {}, everything=True)
