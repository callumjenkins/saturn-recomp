# rcheevos, the runtime and its parser

From rcheevos (https://github.com/RetroAchievements/rcheevos), MIT (`LICENSE`), at commit
`eea6461f172d37d36b0d56568f13a505e0a9fd86` (2026-11-05), unchanged. These are only the files
`rc_runtime_activate_achievement` and `rc_runtime_do_frame` need:

- `include/`: `rc_runtime.h`, `rc_error.h`, `rc_export.h`, `rc_util.h`
- `src/runtime/`: the condition parser and evaluator (`rc_alloc` to `rc_value`), with the leaderboard
  and rich presence code that `rc_runtime.c` calls
- `src/util/`: `md5`, `rc_compat`, `rc_util`

Left out: `rc_client`, `rc_api`, `rc_hash`, `rc_libretro`, the validator, and progress
serialisation. Nothing here makes a network call.

To update, copy the same files from a newer commit and change the commit above and in
`THIRD_PARTY.md`.
