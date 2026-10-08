# Persistent save data

Save data is separate from scene edits and `game.get/set` session data. Each engine
owns a memory store by default, including render, script, MCP and test sessions.
An explicit `--save-dir` enables JSON files outside the project, without exposing
arbitrary file paths through commands or Lua. Slots are independent objects.

## Status

- [ ] Android save/restart on a device (force-stop, relaunch, check the slot JSON).
- [ ] Native Linux full build/runtime check.

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

While a network match or `sim.record_state` recording is active, persistence is
deferred so replayed frames cannot write twice: `save.flush` succeeds, keeps the
slot dirty and remembers the request. The remembered slots are written when the
match/recording ends (`sim.stop`, or the next `net.host`/`net.join`); a failed
write there is logged and the slot stays dirty for a later `save.flush`.
