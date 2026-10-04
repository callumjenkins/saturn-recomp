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
3. **Done.** The game config file. A game's `game.toml` names its modules, symbols, task switch,
   hooks and learned-seeds file (`src/saturnrecomp/config.py` documents the format).
   - `python -m saturnrecomp.build GAME.toml` recompiles and builds from it. The recompiler compiles
     the task switch into the build (`g_sh2_tasks`), so the runtime needs no `--tasks`.
   - `python -m saturnrecomp.learn GAME.toml -- ARGS` is the run-and-seed loop each game had its
     own copy of.
4. **Done.** Tests and CI. `tools/check.sh` runs everything that needs no game data, and the
   GitHub workflow runs it on every push:
   - pytest (`tests/`): the decoder, discovery on hand-assembled programs, resume points, `game.toml`.
   - The instruction self-test.
   - The runtime's unit tests (`runtime/tests/`), starting with VDP2 composition.
   - All built with `host_null.cpp` in place of SDL, which a build also falls back to when SDL3 is
     missing.
5. **Done, with step 3.** Resume points in the recompiler. `saturnrecomp.analysis.resume` finds them
   from the config's `[tasks]`, and `saturnrecomp.build` adds them to that module's seeds.

## Not planned

- Turning the runtime's global state into objects in one pass. Parts become pure functions as tests
  need them.
- Keeping compatibility with saturnkit's names. vs-sr-dev's ports stay on saturnkit.
