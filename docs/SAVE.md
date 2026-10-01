# Persistent save data

Save data is separate from scene edits and `game.get/set` session data. Each engine
owns a memory store by default, including render, script, MCP and test sessions.
An explicit `--save-dir` enables JSON files outside the project, without exposing
arbitrary file paths through commands or Lua. Slots are independent objects.

## Implementation status (work log)

- [x] P1.1: Memory/directory stores, validated JSON values and slots, atomic file
      replacement, command and Lua APIs, CLI opt-in, restart/corruption tests.
      Windows Release build and all 65 tests pass, including three save tests
      and TemplateGamePlaythrough. CLI script/exec across processes restores
      score 123; default exec remains empty in memory mode. Existing editor
      C4458 warnings remain. Non-Windows build checks follow in P1.2.
- [ ] P1.2: Player defaults: Windows user data, Android internal data, browser
      localStorage; platform build and runtime verification.
- [ ] P1.3: User documentation, regenerated API, optional template high score,
      refreshed prebuilt runtimes where toolchains exist; record remaining checks.

## Contract

`save.get(key, default?, slot?)`, `save.set(key, value, slot?)`,
`save.delete(key, slot?)` and `save.flush(slot?)` use slot `default` when omitted.
Slots contain 1–64 lowercase ASCII letters, digits, underscores or hyphens. Each
disk slot is `slot-<slot>.json` (the prefix avoids reserved Windows filenames).
Keys are nonempty UTF-8 strings. Values must be finite JSON data;
Lua functions, userdata, threads, cycles and tables with mixed/non-string object
keys are rejected. Dense integer-keyed arrays are supported. `nil` is JSON null;
use `save.delete` to remove a key.

Changes remain in memory until `save.flush` (or the `save.flush` command). A flush
writes a temporary sibling file and atomically replaces the destination; a failed
write leaves the previous save intact and returns an error. Invalid save files
are ignored with a warning. Saving does not enter scene undo history. Tool state
commands report the mode, slot, data and whether a flush is pending.
