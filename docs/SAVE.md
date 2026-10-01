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
- [x] P1.2: Player defaults: Windows user data, Android internal data, browser
      localStorage; platform build and runtime verification.
      Windows player launches restore counters 0/1 then 1/2. Browser reload
      restores 0/1 then 1/2 (visible HUD and Lua logs). Windows tests pass;
      WebAssembly and Android arm64-v8a/x86_64 Release players build. Null
      backend passes a Clang C++17 syntax/warnings check using the NDK toolchain.
- [x] P1.3: User documentation, regenerated API, optional template high score,
      refreshed prebuilt runtimes where toolchains exist; record remaining checks.
      Template/sample records survive Play Again and sim.stop; the HUD was
      inspected after collecting three coins (Best: 3). Windows 65 tests pass;
      WebAssembly/Node 61 tests pass (GPU comparisons skip without a canvas).
      The web and both Android prebuilt runtimes are refreshed.
- [ ] Android save/restart on a device: no adb device was connected. Install a
      save-probe APK, force-stop/relaunch, and check the internal slot JSON.
- [ ] Native Linux full build/runtime: only the null source syntax check ran.

## Player defaults

Packaged players enable persistence before scripts start. Windows stores files in
`%APPDATA%/<gameName>/`; Android uses `internalDataPath/saves/<gameName>/`; the null
backend uses `$XDG_DATA_HOME/<gameName>/` or `$HOME/.local/share/<gameName>/`. Unsafe
path characters in the project name are percent-encoded. Web saves use the
origin's localStorage with key `ownengine.save:<gameName>:<slot>`. Games sharing
an origin must use distinct project names for separate save namespaces. Storage
access/quota errors are reported; pending writes remain dirty and can be retried.

There is no implicit flush on shutdown: call `save.flush` at checkpoints. Browser
tabs and mobile processes can disappear without a normal shutdown callback.
The default coin-game template saves `highScore` whenever the cross-scene total
sets a new record and shows `Best` in its HUD. Play Again resets session total
while retaining the record. Failed flushes log a warning and gameplay continues.

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
