# OnlineRPG

`games/OnlineRPG` is the beginning of an online RPG: an HD-2D world behind a login server, a channel
server and game servers. How to run it and which file does what: [games/OnlineRPG/README.md](../games/OnlineRPG/README.md).
This file is the design and, at the end, the work log.

## What it is built from

- **Look**: the HD-2D recipe of `samples/HD2D` — lit, shadow-casting pixel-art sprites (32 pixels per
  world unit) on textured 3D ground and blocks, a 30° lens from above, depth of field focused on the
  player, bloom, vignette, Reinhard tone mapping ([RENDERING.md](RENDERING.md), [POSTPROCESS.md](POSTPROCESS.md)).
  The camera never turns, so sprites keep a fixed rotation and no billboard script runs.
- **Network**: `network.mode: "authoritative"` with `lateJoin` ([NETWORK.md](NETWORK.md)). Every game server is
  `oe serve-game` on its own port; clients send only their declared inputs (`W A S D`, arrows, `Shift`,
  `E`, the left stick) and messages, and receive snapshots of the replicated entities.
- **Rules in Lua** (DESIGN.md §2): everything below is project script; the engine was only changed
  where a script could not do it (see "Engine changes").

## Servers

Since M3 the game has a real back end: programs of their own in `games/OnlineRPG/server/`, designed
in [ONLINERPG_SERVERS.md](ONLINERPG_SERVERS.md).

```
player ──Login──> oe_login (7001) ── account, name, session ticket ──> player
player ──ticket──> oe_channel (7002) ── channel list; Enter ── address + entry ticket ──> player
player ──net.join, name = entry ticket──> game server (oe serve-game, 7778 = Channel 1, 7779 = Channel 2)
                                              └─ asks oe_channel (7003) whose ticket it is, then net.admit
```

| Program | What it does |
|---|---|
| `oe_login` | Accounts in SQLite (guests for now from the game), signed session tickets. |
| `oe_channel` | Knows the game servers and how full they are, hands out entry tickets, answers a game server's question about a ticket. |
| `oe serve-game games/OnlineRPG --port N --set channel=C ...` | One copy of the world. Runs with or without players (`network.lateJoin`); a player's avatar spawns when the channel server vouched for its ticket (`network.admission: "script"`, `scripts/lib/gate.lua`). |

The client's part is `scripts/connect.lua`. Entering a world restarts the client's scripts from the
server's state, so "online" is simply "a world is running"; when that stops without the player
leaving, a modal popup says so and its OK button calls `game.quit()`. A server started by hand
without the back end (`oe serve-game games/OnlineRPG`, an editor preview) admits everybody under
the name they joined with.

## Design of the world

| Thing | Where it lives | How peers learn about it |
|---|---|---|
| Player avatar | `prefabs/player.prefab.json`, spawned for each player that enters (`network.playerPrefab`) | `NetSync` replicates `Transform.position`; the owner predicts with the same `player.lua` |
| Walk cycle, facing, name | `player.lua` writes `"walk_left,Guest0001"` into the avatar's `Tag` | `NetSync` replicates `Tag.tags`; `hud.lua` sets the `Art` child's clip and flip and the name plate from it |
| Ground item | `prefabs/items/<key>.prefab.json`, instantiated/destroyed by the server | spawn/despawn records in snapshots (`NetSync.prefab`) |
| Bag (inventory) | a Lua table on the server, 8 slots, stacks of 99 | message `inv(bag)` to its owner after every change |
| Chat line | — | request `chat(text)` → the server checks it → `say(player, name, text)` to everybody |
| Speech bubble, name plate | eight pre-placed UI pairs, handed to the avatars on screen every frame by `hud.lua` | derived locally from positions and `say` messages; never replicated |

Server-side checks (`scripts/game.lua`): a chat line is a valid UTF-8 string of 1..80 characters
without control characters, at most one per 30 frames per player; a drop names an existing slot of
the sender's own bag; a pick-up takes the nearest item within 1.3 units of the sender's avatar and
fails when the bag is full; at most 64 items lie on the ground. The sender is always `net.sender()`,
never an argument.

Walking is plain arithmetic on a flat world (`world.move`: bounds, circles for trees and lamps, a box
for the pond) instead of physics bodies, so the predicted and the server position agree exactly and
the world needs no colliders. `tools/make_scene.py` writes the obstacle table next to the entities it
stands for.

The walk cycle is replicated state, not a guess from how an avatar's position changed: a correction
replays the client's unacknowledged frames with remote avatars at their newest snapshot position
and then shows them at the interpolated one, so a per-frame position difference points backwards
about twenty times a second (the sprite flickered between left and right in the first version).
More generally, a local script must not integrate remote entities' motion frame by frame.

Player numbers are session ids that only grow on a long-running server (2, 3, ... for everybody who
ever joined), so nothing is indexed by them: spawn points and tints wrap around, name plates are
handed out per frame.

`scripts/lib/session.lua` hides the difference between offline and a session: `session.authority()` is
true where the rules run, `session.send` calls the handler directly when there is no session, and
`session.input(number)` is a player's keys either way.

How the scripts are laid out, so they read top-down:

| Helper | What it gives a script |
|---|---|
| `scripts/lib/phases.lua` | A state machine: `phases.new(self, Phase)`, each phase a table with `enter(self, ...)` / `update(self)`; `go(name, ...)`, `is(name)`, `frames()`. `connect.lua` is one block per phase (title, offline, login, lobby, entering, online, popup) plus a `Reply[phase][message type]` table for server messages. |
| `scripts/lib/widgets.lua` | UI handles: `widgets.find("Popup")` / `widgets.list("Name ", 8)`, then `:text(line)`, `:show(on)`, `:hide()`, `:visible()`, `:set(values)`, `:focus()` instead of `scene.set(id, "UIText", ...)`. |
| `scripts/lib/space.lua` | `space.at(id)`, `space.distance2/within` on the ground plane, `space.project(camera, x, y, z)` to UI pixels. |
| `scripts/lib/platform.lua` | Besides the message numbers, the decoders of what a client receives (`loginResult`, `channels`, `enterResult`, `errorCode`), so `connect.lua` holds no `string.unpack` formats. |

`hud.lua` and `game.lua` are split into named steps (`dress`, `label`, `forget`; `populate`, `welcome`,
`nearestItem`) with `onUpdate` as the list of them.

Speech bubbles are `UIPanel` + `UIText` pairs moved every frame to the avatar's head projected to the
screen in Lua (`space.project`; the camera only pitches, so it is a few lines). Latin letters count as
0.55 em and Hangul/kana as 1 em to size the bubble.

## Engine changes

Made for this game, usable by any authoritative game (details in [NETWORK_NOTES.md](NETWORK_NOTES.md),
audit follow-up):

- **Local input in an authoritative match.** A client's UI and local scripts get the device's pointer,
  typed text and undeclared keys (never sent); while a `UIInput` has the focus the keyboard stays out
  of the network input stream, so typing "wasd" into the chat does not walk.
- **Late join** (`network.lateJoin: true`). A dedicated server starts its world at once and keeps it
  running without players; a player that joins the lobby and gets ready is admitted into the running
  match (the same Begin / Ready / Go barrier, for that player alone), gets its `playerPrefab` avatar and
  a full snapshot; leaving frees the avatar, and the same player can come back.
- **A stopped match no longer freezes the peer.** When the server goes away the client simulates
  locally again, so scripts can react (`onNetState`, a popup). Before, no script ran after the stop.
- **`game.quit()`** (Lua and command) asks the game window to close; **`net.name(player)`** gives the
  name a player joined with.
- A loopback host that had run for 600 frames dropped every new loopback peer as timed out
  (found by the late-join test; real sockets were not affected).

## Limits and next steps

- **Nothing of the world is saved yet.** Bags and ground items live in a game server's Lua state
  until that server stops, and a player starts fresh on every visit (ONLINERPG_SERVERS.md S4).
- **The game only logs in as a guest**: accounts with passwords exist in `oe_login`, the screens for
  them and the channel list come with S5.
- **Changing channel or map** in the game is not built (S7).
- **64 participants** per server process (engine limit), 8 configured here (7 players per channel).
- Korean text can be typed on Windows (committed IME text; no composition preview) but not on
  Android or the web player yet (UI.md work log).
- Item art is procedural pixel art from `tools/make_art.py`; the world art is shared with
  `samples/HD2D`. Replace both when the game gets its own art direction.

## Implementation status (work log)

- [x] M1 — HD-2D world, walking with prediction, ground items, pick-up, bag, drop, chat, speech
      bubbles, name plates; engine: local input in authoritative matches. Walk cycles replicated
      (fixes sprites of other players flickering between left and right).
- [x] M2 — login server + channel servers with automatic hand-over, players entering and leaving a
      running world, names from the login server, title screen, disconnect popup whose OK closes the
      game; engine: `network.lateJoin`, peers keep simulating after a stopped match, `game.quit`,
      `net.name`. Verified on Windows: `oe_tests` (196 tests: `NetworkAuthoritativeLateJoin`,
      `OnlineRPGGame`), `python tests/online_rpg_test.py` (real login + two channel processes and
      client processes over TCP: hand-over, late join, pick up, Korean chat, drop, a player leaving
      and another arriving, popup + quit request after killing the channel, fallback to Channel 2),
      GPU screenshots of the title and the popup; a real `oe run` window exits when `game.quit` is
      called (through its `--port` API). Web runtime rebuilt (Emscripten 6.0.10).
      Not verified: windows played by hand (clicking Login and the popup's OK with a mouse),
      the browser client, LAN/other machines, a full channel (8 players), latency/loss.
- [x] M3 — the back end as separate programs (`games/OnlineRPG/server/`: `oe_login`, `oe_channel`) and
      the game on tickets instead of ports: work log in ONLINERPG_SERVERS.md (S1 engine support, S2
      login server, S3 channel server and the game's new way in). `tests/online_rpg_test.py` now
      drives the real back end.
- [ ] Android runtimes (`runtime/android/`) were not rebuilt for the engine changes: run
      `build_android.bat` with `ANDROID_NDK_HOME` set and commit the refreshed libraries.
- [ ] Persistence, login screens, channel list, map and channel change: ONLINERPG_SERVERS.md S4-S7.
- [ ] Own art: characters with more directions, item icons, a tileset for larger maps.

## Lua widget pilot (M1b, 2026-10-06)

scripts/lib/widgets.lua now returns require("oe.ui"), retaining the project import path
for hud.lua/connect.lua callers. Official widgets keep selected-component get/set,
text, show/hide/visible, focus and numbered lists, and add identity/capability/ambiguity
validation. Per-instance widget acquisition in onStart is retained; official handles
never silently re-find after a world replacement. No projection/bubble-width formula,
network gameplay rule, bag/chat action or server code was changed.

All 50 primary UI entities in the starting scene lack NetSync; authoritative replication
still enumerates NetSync entities only. Native client pointer/text/undeclared-key input
continues to drive local UI. HUD's pre-existing cosmetic/avatar and debug game-data writes
remain; this is not a claim of universal hash exclusion for HUD scripts.
Title/offline HUD pixel hashes and PNG bytes match before migration. OnlineRPGGame and
the full 223-test desktop suite pass, as does tests/online_rpg_test.py with actual login,
channel and game processes (backend build successful). Details in LUA_USABILITY.md 6.3/8.
Web and both Android runtimes were refreshed; device/browser execution of M1b remains
unverified. Prior Android rebuild deferrals are superseded for compilation, not device tests.

## Coordinate pilot (M2a, 2026-10-06)

Space.project uses explicit-camera worldToScreen and native screenToCanvas instead of
pitch-only projection and a hard-coded reference-height calculation. Nameplate and bubble
callers keep the same API and depth<0.1 suppression. Offscreen positions remain unclamped.
The current scene has no UICanvas, so native height-720 fallback matches the old policy even
at a 900-pixel view height: title, HUD, bubble and resized-view hashes remain identical.
Rotated/parent cameras and effective UICanvas scaling are now supported by renderer matrices.
Text-width ems(), bag/chat/network rules, and server code are unchanged. Queries place local
UI only; the legacy cosmetic/debug game-data writes are not newly excluded from state hashes.
See LUA_USABILITY.md 6.4/8 for visual, end-to-end and rebuilt-runtime evidence.

## Text measurement pilot (M2b, 2026-10-06)

HUD bubbleSize uses ui.measureText for natural width and actual wrapped height instead of
ems/category counts and estimated row count. Maximum width 216 and existing 8/24/18 padding
remain project policy. Visible bubbles remeasure with their actual label font/style and
current view scale, so cached message dimensions do not outlive a resize incorrectly.
English wrapped height and Korean one-line height become tighter. Japanese fallback boxes
are measured correctly but Japanese glyph coverage remains absent. Message life, chat/server
rules, authoritative movement/inventory and text colors are unchanged. See LUA_USABILITY.md
6.5/8 for before/after classifications, full desktop/Wasm and process/browser evidence.
