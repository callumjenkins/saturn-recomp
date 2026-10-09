"""A game's game.toml: the programs to recompile, names for addresses, the task switch and hooks.

    [game]
    name = "Saturn Bomberman (USA)"
    seeds = "tools/seeds.json"      # seeds the learning loop found, by module
    build = "build"
    cmake = ["SATURN_VDP1_GPL=ON"]  # optional: the runtime's build options (runtime/runtime.cmake)

    [symbols]
    krnl_setjmp = 0x060061C4

    [[module]]
    name = "KRNL"
    file = "build/extract/BOMSS/0KRNL.BIN"
    base = 0x06006000
    seeds = [0x06006100]            # optional

    [tasks]                         # optional: a game that switches its own tasks
    module = "KRNL"
    setjmp = "krnl_setjmp"
    longjmp = "krnl_longjmp"
    yields = ["krnl_setjmp", "krnl_yield"]
    unknown_yields = true           # a call to a target discovery cannot name may yield too

    [[hook]]
    module = "KRNL"
    at = "bomber_hit_flag"

    [playtest]                      # optional: builds for playtesters (saturnrecomp.playtest)
    endpoint = "https://saturn-playtest.example.workers.dev"
    repo = "OWNER/NAME"             # the GitHub repository whose releases hold the builds
    disc = "disc.json"              # the supported disc's manifest (saturnrecomp.disc --manifest)
    coverage = ["build/test/*/coverage.txt"]   # the runs a session's new code is measured against
    args = []                       # saturn arguments every session plays with
    boxart = "https://..."          # the box art the Android app shows, fetched by the app, never bundled

    [checkpoint]                    # optional: a start a session's Continue can rebuild when its dump won't load
    keep = [["stage_2", 2], ["score", 8], ["objects+0x5E", 1]]   # the first's changes start one
    accept = [[0x0001, 0x0006], [0x0100, 0x0108]]   # ranges of the first's values, big-endian, it can be rebuilt from
    presses = ["1900:START", "1910:"]   # from power-on to where the writes go in
    writes = ["4250:stage=stage_2", "4600:score", "4600:objects+0x5E&08"]   # AT:TARGET[=KEPT][&MASK]
    resume = 4610                   # the player's pads take over here
    fresh = [0x0000]                # the first's values a new game starts at: a save file starts there
    show = [["Stage {}", "stage_2", "world-stage"], ["{} pts", "score", "u32"],
            ["Speed +{}", "objects+0x3A", "steps:E0:20?"], ["{}", "objects+0x70", "bits:01=Kick,02=Glove"],
            ["{} dino", "dino_colours", "names:,Pink,Blue", "objects+0x5E&08"]]

A restore point shows each of `show` it can, as TEMPLATE, KEPT, FORMAT and optionally a condition, a
kept range whose first byte must have a bit of the mask. The formats read the range's first bytes,
big-endian: u8, u16, u32; world-stage, a byte each, numbered from 1; steps:BASE:STEP, how many STEPs
the byte is above BASE, wrapping at 256; names:A,B,... the byte's name, from 0; bits:MASK=NAME,...
the names of the first byte's bits. A trailing "?" leaves out a 0 (or no name).

An address is a number, a name from [symbols], or either plus an offset ("objects+0x5E"). Paths are
relative to the file.
"""
import json
import os
import tomllib
from dataclasses import dataclass, field


@dataclass
class Module:
    name: str
    file: str
    base: int
    seeds: list[int] = field(default_factory=list)


@dataclass
class Tasks:
    module: str
    setjmp: int
    longjmp: int
    yields: list[int]
    unknown_yields: bool = False


@dataclass
class Playtest:
    endpoint: str
    repo: str
    disc: str
    coverage: list[str] = field(default_factory=list)
    args: list[str] = field(default_factory=list)
    boxart: str = ""

    @property
    def manifest(self):
        return json.load(open(self.disc))

    @property
    def product(self):
        """The disc's product number and version as the runtime names its save directory, such as MK-81070_V1.003."""
        m = self.manifest
        return f"{m['product']}_{m['version']}"


@dataclass
class Checkpoint:
    """Ranges a session notes whenever the first changes (the runtime's --progress), and how a start is
    rebuilt from a line of them: presses from power-on, then the kept bytes written back."""
    keep: list[tuple[int, int]]                 # (address, bytes)
    accept: list[tuple[int, int]]
    presses: list[str]
    writes: list[tuple[int, int, int, bytes | None]]   # (VBlank, address, index into keep, mask)
    resume: int
    fresh: list[int] = field(default_factory=list)
    show: list[tuple[str, int, str, tuple[int, int] | None]] = field(default_factory=list)   # (template, kept, format, (kept, mask))

    @property
    def keep_arg(self):
        return ",".join(f"{a:08X}:{n}" for a, n in self.keep)

    def rebuild(self, line):
        """The --write arguments that rebuild the start a --progress line names, or None when its first
        range is outside `accept`."""
        fields = line.split()
        kept = [bytes.fromhex(f) for f in fields[1:]]
        if len(kept) != len(self.keep) or not any(lo <= int.from_bytes(kept[0], "big") <= hi for lo, hi in self.accept):
            return None
        out = []
        for at, addr, k, mask in self.writes:
            data = kept[k] if mask is None else bytes(b & m for b, m in zip(kept[k], mask))
            out.append(f"{at}:{addr:08X}={data.hex().upper()}")
        return out

    def describe(self, line):
        """What a --progress line shows of the game, by `show`: a phrase each, as the launcher shows them."""
        kept = [bytes.fromhex(f) for f in line.split()[1:]]
        if len(kept) != len(self.keep):
            return []
        out = []
        for template, k, fmt, cond in self.show:
            if cond and not kept[cond[0]][0] & cond[1]:
                continue
            value = show_value(fmt.rstrip("?"), kept[k])
            if fmt.endswith("?") and value in ("", "0"):
                continue
            out.append(template.replace("{}", value))
        return out

    def to_json(self):
        return {"keep": self.keep_arg, "accept": [list(a) for a in self.accept], "presses": ",".join(self.presses), "resume": self.resume,
                "writes": [[at, f"{addr:08X}", k, mask.hex() if mask else None] for at, addr, k, mask in self.writes],
                "fresh": self.fresh, "show": [[t, k, f, list(c) if c else None] for t, k, f, c in self.show]}


def show_value(fmt, data):
    """A kept range as one of [checkpoint] show's formats gives it; the launcher's Kotlin does the same."""
    kind, _, arg = fmt.partition(":")
    if kind in ("u8", "u16", "u32"):
        n = int(kind[1:]) // 8
        return f"{int.from_bytes(data[:n], 'big'):,}"
    if kind == "world-stage":
        return f"{data[0] + 1}-{data[1] + 1}"
    if kind == "steps":
        base, step = (int(x, 16) for x in arg.split(":"))
        return str(((data[0] - base) & 0xFF) // step)
    if kind == "names":
        names = arg.split(",")
        return names[data[0]] if data[0] < len(names) else ""
    if kind == "bits":
        return ", ".join(name for bit, name in (b.split("=", 1) for b in arg.split(",")) if data[0] & int(bit, 16))
    raise SystemExit(f"[checkpoint] show: no format {fmt}")


def _checkpoint(c, symbols):
    names = [k[0] for k in c["keep"]]
    keep = [(_addr(name, symbols), n) for name, n in c["keep"]]
    writes = []
    for w in c["writes"]:
        at, rest = w.split(":", 1)
        rest, _, mask = rest.partition("&")
        target, _, kept = rest.partition("=")
        kept = kept or target
        if kept not in names:
            raise SystemExit(f"[checkpoint]: the write {w} names {kept}, which keep does not")
        k = names.index(kept)
        mask = bytes.fromhex(mask) if mask else None
        if mask is not None and len(mask) != keep[k][1]:
            raise SystemExit(f"[checkpoint]: the write {w} has a mask of another length than {kept}")
        writes.append((int(at), _addr(target, symbols), k, mask))
    def kept(name, why):
        if name not in names:
            raise SystemExit(f"[checkpoint]: {why} names {name}, which keep does not")
        return names.index(name)
    show = []
    for s in c.get("show", []):
        template, name, fmt, *cond = s
        show_value(fmt.rstrip("?"), bytes(4))                # a format it doesn't know stops here
        when = None
        if cond:
            what, _, mask = cond[0].partition("&")
            when = (kept(what, f"show {template}"), int(mask or "FF", 16))
        show.append((template, kept(name, f"show {template}"), fmt, when))
    return Checkpoint(keep, [tuple(a) for a in c["accept"]], list(c["presses"]), writes, c["resume"], list(c.get("fresh", [])), show)


@dataclass
class Game:
    name: str
    root: str
    build: str
    seeds_file: str
    modules: list[Module]
    symbols: dict[str, int]
    tasks: Tasks | None
    hooks: dict[str, list[int]]
    cmake: list[str] = field(default_factory=list)
    playtest: Playtest | None = None
    checkpoint: Checkpoint | None = None

    @property
    def saturn(self):
        return os.path.join(self.build, "recomp-build", "saturn")

    def module(self, name):
        return next(m for m in self.modules if m.name == name)

    def learned_seeds(self):
        """{module name: [address, ...]} from the seeds file, empty if there is none yet."""
        if not os.path.exists(self.seeds_file):
            return {}
        return {k: [int(x, 16) for x in v] for k, v in json.load(open(self.seeds_file)).items()}

    def save_learned_seeds(self, seeds):
        with open(self.seeds_file, "w") as f:
            json.dump({k: [f"{x:08X}" for x in v] for k, v in seeds.items()}, f, indent=1)
            f.write("\n")


def load(path):
    path = os.path.abspath(path)
    root = os.path.dirname(path)
    with open(path, "rb") as f:
        t = tomllib.load(f)
    symbols = dict(t.get("symbols", {}))
    modules = [Module(m["name"], os.path.join(root, m["file"]), m["base"],
                      [_addr(s, symbols) for s in m.get("seeds", [])]) for m in t.get("module", [])]
    names = {m.name for m in modules}

    def known(module, where):
        if module not in names:
            raise SystemExit(f"{path}: {where} names module {module}, which no [[module]] defines")
        return module

    tasks = None
    if "tasks" in t:
        k = t["tasks"]
        tasks = Tasks(known(k["module"], "[tasks]"), _addr(k["setjmp"], symbols), _addr(k["longjmp"], symbols),
                      [_addr(y, symbols) for y in k.get("yields", [])], k.get("unknown_yields", False))
    hooks = {}
    if tasks:
        hooks.setdefault(tasks.module, []).extend([tasks.setjmp, tasks.longjmp])
    for h in t.get("hook", []):
        hooks.setdefault(known(h["module"], "a [[hook]]"), []).append(_addr(h["at"], symbols))
    game = t.get("game", {})
    playtest = None
    if "playtest" in t:
        p = t["playtest"]
        playtest = Playtest(p["endpoint"].rstrip("/"), p["repo"], os.path.join(root, p.get("disc", "disc.json")),
                            [os.path.join(root, g) for g in p.get("coverage", [])], list(p.get("args", [])),
                            p.get("boxart", ""))
    return Game(game.get("name", os.path.basename(root)), root, os.path.join(root, game.get("build", "build")),
                os.path.join(root, game.get("seeds", "seeds.json")), modules, symbols, tasks, hooks,
                list(game.get("cmake", [])), playtest, _checkpoint(t["checkpoint"], symbols) if "checkpoint" in t else None)


def _addr(value, symbols):
    if isinstance(value, int):
        return value
    name, plus, offset = value.partition("+")
    if name not in symbols:
        raise SystemExit(f"no symbol named {name}")
    return symbols[name] + (int(offset, 0) if plus else 0)
