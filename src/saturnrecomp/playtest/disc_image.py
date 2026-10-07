"""The tester's own disc image, from a file or a URL they give, as the .cue the runtime opens."""
import os
import re
import urllib.parse
import urllib.request
import zipfile

from .. import disc
from .upload import USER_AGENT

CHUNK = 1 << 20


def cue_for(image, folder):
    """A .cue for `image`: itself if it is one, or one written into `folder` naming a lone .iso or .bin."""
    if image.lower().endswith(".cue"):
        return image
    with open(image, "rb") as f:
        raw = f.read(12) == b"\x00" + b"\xff" * 10 + b"\x00"
    os.makedirs(folder, exist_ok=True)
    cue = os.path.join(folder, "disc.cue")
    with open(cue, "w") as f:
        f.write(f'FILE "{os.path.abspath(image)}" BINARY\n'
                f"  TRACK 01 {'MODE1/2352' if raw else 'MODE1/2048'}\n"
                "    INDEX 01 00:00:00\n")
    return cue


def download(url, folder, progress=None):
    """The image at `url`, saved into `folder` and unpacked if it is a .zip; the path of its .cue, .iso or .bin.

    `progress(done, total)` is called as it comes, with `total` 0 when the server does not say.
    """
    os.makedirs(folder, exist_ok=True)
    with urllib.request.urlopen(urllib.request.Request(url, headers={"user-agent": USER_AGENT}), timeout=60) as r:
        name = _file_name(url, r.headers.get("content-disposition", ""))
        path = os.path.join(folder, name)
        total = int(r.headers.get("content-length") or 0)
        done = 0
        with open(path + ".part", "wb") as out:
            while chunk := r.read(CHUNK):
                out.write(chunk)
                done += len(chunk)
                if progress:
                    progress(done, total)
    os.replace(path + ".part", path)
    if not zipfile.is_zipfile(path):
        return path
    unpacked = os.path.join(folder, os.path.splitext(name)[0])
    with zipfile.ZipFile(path) as z:
        for member in z.namelist():
            target = os.path.realpath(os.path.join(unpacked, member))
            if not target.startswith(os.path.realpath(unpacked) + os.sep):
                raise ValueError(f"the zip names a file outside itself: {member}")
        z.extractall(unpacked)
    os.remove(path)
    return find_image(unpacked)


def find_image(folder):
    """The disc image in `folder`: its .cue, or else its one .iso or .bin."""
    found = [os.path.join(dp, f) for dp, _, fs in os.walk(folder) for f in fs]
    for ext in (".cue", ".iso", ".bin"):
        hits = [p for p in found if p.lower().endswith(ext)]
        if len(hits) == 1 or (hits and ext == ".cue"):
            return sorted(hits)[0]
    raise ValueError("no .cue, and not exactly one .iso or .bin, in what was downloaded")


def verify(image, manifest):
    """(errors, warnings) against the supported disc: errors stop play, warnings mean only the music differs."""
    return disc.check(image, manifest)


def _file_name(url, disposition):
    m = re.search(r'filename="?([^";]+)"?', disposition)
    name = m.group(1) if m else os.path.basename(urllib.parse.urlparse(url).path)
    name = re.sub(r"[^\w.-]", "_", urllib.parse.unquote(name))
    return name if name.strip(".") else "disc.bin"
