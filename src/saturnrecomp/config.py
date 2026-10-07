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

An address is a number or a name from [symbols]. Paths are relative to the file.
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
                list(game.get("cmake", [])), playtest)


def _addr(value, symbols):
    if isinstance(value, int):
        return value
    if value not in symbols:
        raise SystemExit(f"no symbol named {value}")
    return symbols[value]
