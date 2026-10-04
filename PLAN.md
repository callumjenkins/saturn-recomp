# Plan: saturn-recomp's structure

saturn-recomp came from saturnkit with saturnkit's flat layout. This plan moves it to the shape the
larger recompilation projects use, without changing what any game does. Each step is checked by the
SH-2 instruction self-test and by pc-saturnbomberman's frame test (`tests/frames.py`, nine scripted
runs compared frame by frame). Both have to pass before a step is done.

The decisions behind it are ADRs in the agent home, under `projects/saturn-recomp/adrs/`.

## The shape

N64Recomp, N64ModernRuntime and Zelda64Recomp split the work into three parts: a translator that
knows no game, a runtime that replaces the console, and a game repository that holds a config file,
symbols and patches. saturn-recomp holds the first two, and each game is a repository of its own.

```
saturn-recomp/
  pyproject.toml          the Python package, installed editable by each game
  src/saturnrecomp/       everything that runs before the build: disc, sh2, hw, recomp
  runtime/include/saturn/ the API recompiled code and game layers see
  runtime/src/            core, machine, video, sound, host (the only SDL code), app, stub
  tests/                  tests that need no game data
  tools/ghidra/
```

## Steps

1. **Done.** Python under `src/saturnrecomp` with `pyproject.toml`. CMake targets renamed
   `saturn_*`. pc-saturnbomberman takes saturn-recomp as a submodule, installed through uv.
2. **Done.** The runtime split into `include/saturn` and `src/<subsystem>`. The host interface is in
   `host.h`, and PNG writing is in `video/png.cpp`. `saturn_machine` builds without SDL.
3. **The game config file.** A game's `game.toml`:

   ```toml
   [game]
   name = "Saturn Bomberman (USA)"
   seeds = "tools/seeds.json"          # written by the seed-learning loop

   [[module]]
   name = "KRNL"
   file = "build/extract/BOMSS/0KRNL.BIN"
   base = 0x06006000

   [symbols]
   krnl_setjmp = 0x060061C4
   krnl_longjmp = 0x060061E6
   krnl_yield = 0x06006D36

   [tasks]                             # compiled into the build; resume points found from it
   setjmp = "krnl_setjmp"
   longjmp = "krnl_longjmp"
   yields = ["krnl_setjmp", "krnl_yield"]

   [[hook]]
   at = 0x0606CF2E
   name = "invincible_hit"
   ```

   - `python -m saturnrecomp.recomp --config game.toml` replaces the module and hook arguments.
   - `python -m saturnrecomp.learn` replaces each game's copy of the seed-learning loop.
4. **Tests and CI.**
   - pytest for the decoder and discovery on hand-assembled snippets.
   - A null host, so the runtime builds and runs with no SDL.
   - Unit tests for chips whose output is a function of their inputs, starting with VDP2
     composition.
   - A GitHub Actions job running all of it plus the instruction self-test.
5. **Resume points in the recompiler.** `resume_points.py`'s analysis moves into
   `saturnrecomp.analysis`, and the recompiler runs it whenever the config names a task switch.

## Not planned

- Turning the runtime's global state into objects in one pass. Parts become pure functions as tests
  need them.
- Keeping compatibility with saturnkit's names. vs-sr-dev's ports stay on saturnkit.
