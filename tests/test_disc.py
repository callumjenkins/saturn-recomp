import json
import os
import struct

from saturnrecomp import disc

FILES = {"A.BIN": b"first read" * 300, "DATA.DAT": bytes(range(256)) * 20}


def ip(product=b"T-0000    ", version=b"V1.000"):
    h = b"SEGA SEGASATURN " + b"SEGA TP T-000   " + product + version + b"19951215" + b"CD-1/1  "
    h += b"U".ljust(16) + b"J".ljust(16) + b"TEST".ljust(0x70)
    h += struct.pack(">6I", 0x100, 0, 0x06001000, 0x06000800, 0x06004000, 0)
    return h.ljust(16 * 2048, b"\0")


def record(name, lba, size, is_dir=False):
    pad = b"\0" if len(name) % 2 == 0 else b""
    body = (b"\0" + struct.pack("<II", lba, 0) + struct.pack("<II", size, 0) + bytes(7) +
            bytes([2 if is_dir else 0, 0, 0]) + bytes(4) + bytes([len(name)]) + name + pad)
    return bytes([len(body) + 1]) + body


def image(tmp_path, files=FILES, product=b"T-0000    ", audio=b"\x01\x02" * 2352):
    """A disc as a .cue: an ISO 9660 data track holding `files`, and one audio track."""
    lba = 19
    root = record(b"\0", 18, 2048, True) + record(b"\1", 18, 2048, True)
    data = b""
    for name, body in files.items():
        root += record(name.encode() + b";1", lba, len(body))
        data += body.ljust(-(-len(body) // 2048) * 2048, b"\0")
        lba += -(-len(body) // 2048)
    pvd = bytearray(2048)
    pvd[0:7] = b"\1CD001\1"
    pvd[80:84] = struct.pack("<I", lba)
    pvd[156:190] = record(b"\0", 18, 2048, True)
    track = ip(product) + bytes(pvd) + bytes(2048) + root.ljust(2048, b"\0") + data
    (tmp_path / "a.iso").write_bytes(track)
    (tmp_path / "b.bin").write_bytes(audio)
    (tmp_path / "game.cue").write_text('FILE "a.iso" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n'
                                       'FILE "b.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n')
    return str(tmp_path / "game.cue")


def expected(tmp_path):
    good = tmp_path / "good"
    good.mkdir()
    return disc.manifest(disc.Disc(image(good)))


def test_the_same_disc_matches(tmp_path):
    want = expected(tmp_path)
    assert set(want["files"]) == {"/A.BIN", "/DATA.DAT"} and set(want["audio"]) == {"02"}
    assert disc.check(image(tmp_path), want) == ([], [])


def test_a_changed_file_is_named(tmp_path):
    want = expected(tmp_path)
    files = dict(FILES, **{"DATA.DAT": b"\xff" + FILES["DATA.DAT"][1:]})
    assert disc.check(image(tmp_path, files), want) == (["/DATA.DAT differs"], [])


def test_a_truncated_data_track_fails(tmp_path):
    want = expected(tmp_path)
    cue = image(tmp_path)
    iso = tmp_path / "a.iso"
    iso.write_bytes(iso.read_bytes()[:-2048])
    errors, _ = disc.check(cue, want)
    assert errors == ["cannot be read: /DATA.DAT: sector 23 is past the end of the data track"]


def test_another_revision_is_refused_by_its_header(tmp_path):
    want = expected(tmp_path)
    errors, _ = disc.check(image(tmp_path, product=b"T-0001    "), want)
    assert errors == ["this disc is T-0001 V1.000, not T-0000 V1.000"]


def test_files_added_or_removed_are_errors(tmp_path):
    want = expected(tmp_path)
    files = {"A.BIN": FILES["A.BIN"], "NEW.DAT": b"x"}
    errors, _ = disc.check(image(tmp_path, files), want)
    assert errors == ["/DATA.DAT is missing", "/NEW.DAT is not on T-0000 V1.000"]


def test_a_missing_track_file_is_named(tmp_path):
    want = expected(tmp_path)
    cue = image(tmp_path)
    os.remove(tmp_path / "b.bin")
    errors, _ = disc.check(cue, want)
    assert errors == ["cannot be read: track 02's file 'b.bin' is missing"]


def test_a_different_audio_track_is_a_warning(tmp_path):
    want = expected(tmp_path)
    assert disc.check(image(tmp_path, audio=b"\x03\x04" * 2352), want) == ([], ["audio track 02 differs"])


def test_not_a_disc(tmp_path):
    want = expected(tmp_path)
    (tmp_path / "x.iso").write_bytes(bytes(20 * 2048))
    errors, _ = disc.check(str(tmp_path / "x.iso"), want)
    assert errors == ["cannot be read: not a Saturn disc (no SEGA SEGASATURN header)"]


def test_the_command_line_exits_1_on_an_error(tmp_path, capsys):
    want = tmp_path / "want.json"
    want.write_text(json.dumps(expected(tmp_path)))
    disc.main([image(tmp_path), "--check", str(want)])
    assert "matches" in capsys.readouterr().out
    (tmp_path / "b.bin").unlink()
    try:
        disc.main([str(tmp_path / "game.cue"), "--check", str(want)])
        raise AssertionError("no exit")
    except SystemExit as e:
        assert e.code == 1


def test_a_fingerprint_changes_with_any_of_the_images_files(tmp_path):
    cue = image(tmp_path)
    before = disc.fingerprint(cue)
    assert sorted(os.path.basename(f) for f in before) == ["a.iso", "b.bin", "game.cue"]
    assert disc.fingerprint(cue) == before
    (tmp_path / "b.bin").write_bytes(b"\x05\x06" * 2352)
    os.utime(tmp_path / "b.bin", ns=(1, 1))
    assert disc.fingerprint(cue) != before
