# Playtesting

Builds of a game for other people to play, with every session sent back for review. It works for
any game on saturn-recomp that adds a `[playtest]` table to its `game.toml`.

- **The tester** downloads a release from the game's GitHub releases and runs the launcher, or
  installs the Android app (`android/`). Either one checks their disc, plays the game and sends
  the session to the playtest Worker every 10 minutes and when it ends. Their sessions page shows
  each review once it's done.
- **The Worker** (`worker/`) is one Cloudflare Worker for every game, with sessions in D1 and their
  files in R2, keyed by the disc's product (`MK-81070_V1.003`).
- **The maintainer** runs `python -m saturnrecomp.playtest GAME.toml ...` to invite testers,
  publish builds, and pull, review and mark sessions.

`README.txt` is what testers read. It goes in every release and in its notes.

## What a session is

The launcher runs the game with `--record-input`, `--clock`, `--coverage` and `--checkpoint 600`,
and keeps the saves it started with. That is enough to replay the session exactly on the same build.
The runtime rewrites the coverage every 10 minutes and when the game stops, a fatal error included.
The launcher sends the whole session each time, and each upload replaces the last.

A review measures two things:

- **Missed functions.** When the game calls code that discovery missed, it stops with
  `FATAL] call to ADDR, not an entry`. `review` names the module and the address, and
  `--add-seeds` adds them to the game's seeds, so the next build has the function.
- **New code.** These are code bytes the session ran that no known run had: the game's own runs
  (`coverage` globs in `[playtest]`) and every session already marked reviewed.

## Setting it up once

```sh
cd saturn-recomp/playtest/worker
pnpm install
npx wrangler d1 create saturn-playtest        # put the id it prints into wrangler.jsonc
npx wrangler r2 bucket create saturn-playtest
npx wrangler d1 migrations apply saturn-playtest --remote
npx wrangler secret put ADMIN_TOKEN           # your CLI's token
npx wrangler secret put CI_TOKEN              # the release workflow's token
npx wrangler deploy
```

In the game's repository:

1. Add `[playtest]` to `game.toml` with the Worker's URL (`saturnrecomp.config` documents it).
2. Add `.github/workflows/playtest.yml`, which calls `playtest-release.yml` here. pc-saturnbomberman has one to copy.
3. Set the repository secret `PLAYTEST_CI_TOKEN` to the Worker's `CI_TOKEN`.
4. Make a keystore for the Android app and set `PLAYTEST_ANDROID_KEYSTORE` to its base64 and
   `PLAYTEST_ANDROID_KEYSTORE_PASSWORD` to its password. Android installs a build over the last only
   when the same key signed both, so keep the keystore: without it, testers have to uninstall, and
   lose their saves, to update.

   ```sh
   keytool -genkeypair -keystore playtest.jks -storetype PKCS12 -alias playtest -keyalg RSA \
     -keysize 4096 -validity 10000 -dname "CN=saturn-recomp playtest"
   base64 -w0 playtest.jks | gh secret set PLAYTEST_ANDROID_KEYSTORE
   gh secret set PLAYTEST_ANDROID_KEYSTORE_PASSWORD
   ```
5. Run `python -m saturnrecomp.playtest game.toml register`.

## The loop

```sh
export SATURN_PLAYTEST_ADMIN_TOKEN=...                       # the Worker's ADMIN_TOKEN
python -m saturnrecomp.playtest game.toml invite "Sam"       # a tester code and their sessions page link
python -m saturnrecomp.playtest game.toml publish            # builds, sends the generated C++, starts the release
python -m saturnrecomp.playtest game.toml pull               # sessions ready for review, into build/playtest/ID
python -m saturnrecomp.playtest game.toml review ID --add-seeds
python -m saturnrecomp.playtest game.toml replay ID --video  # the session again, headless, with a video
python -m saturnrecomp.playtest game.toml reviewed ID @summary.txt
```

`publish` sends the generated C++ to R2 and starts the release workflow through `gh`. The workflow
compiles that C++ against this runtime on Linux, Windows and macOS, packages the launcher with
PyInstaller, builds the Android app for arm64 and makes a GitHub prerelease tagged `playtest-BUILD`. Then it tells the Worker, and the
launchers of older builds point their testers at the new one. CI never sees the disc.

## The Android app

`android/` is a Gradle project that builds one app per game, with its own application id, from the
same `playtest.json` and `disc.json` as the desktop launcher. The launcher activity is Kotlin and
mirrors the desktop launcher, the disc check included. The game runs in SDL's activity in a process
of its own, as `libmain.so`. That activity sends the session while it plays, and the launcher
finishes and sends it once the game's process has gone. The phone's own Back, pressed twice, quits
the game, and a Back from a controller is dropped. "Continue a session" starts a new session from an
earlier one's clock and saves, with the runtime's `--resume` playing its presses again before the
controller takes over, so the new session's `input.txt` replays the whole game. The phone needs a
controller for now. `app/build.gradle.kts` lists the properties the workflow builds it with.

`DiscTest` checks the Kotlin disc check against a real disc. It runs only when `SATURN_CUE` and
`SATURN_DISC_JSON` name the disc and the game's `disc.json`, with `gradle testDebugUnitTest` and the
same `-P` properties as a build.

A session counts as ready for review once it has ended, or once it has sent nothing for 30 minutes,
which is what happens when the launcher is killed along with the game.

## Licences

Release builds leave `SATURN_VDP1_GPL` off, so they are MIT like the rest of the runtime. The
game's own `cmake` options in `game.toml` apply only to builds made with `saturnrecomp.build`.
