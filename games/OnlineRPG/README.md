# OnlineRPG

The start of an online RPG with the HD-2D look of `samples/HD2D` (pixel-art sprites in a lit 3D
world, long lens, depth of field, bloom) on the authoritative network model, with a login server
a channel server and game servers. What exists now: logging in, being sent to a channel, walking, items
lying on the ground, picking them up into a bag, dropping them from the bag, chat between players,
speech bubbles and name plates, and a notice when the connection is lost. Design and work log:
[docs/ONLINERPG.md](../../docs/ONLINERPG.md).

Controls: WASD / arrows / left stick walk, Shift runs, **E** picks up the nearest item, **I** or the
Bag button shows the bag, a click on a bag slot drops one item of it, **Enter** opens the chat field
and sends the line (Escape leaves it).

## Running it

```bat
build.bat
games\OnlineRPG\server\build.bat
games\OnlineRPG\tools\start_servers.bat
build\bin\oe.exe run games/OnlineRPG
```

`start_servers.bat` opens five console windows: the login server (`oe_login`, port 7001), the channel
server (`oe_channel`, 7002 for players and 7003 for game servers) and two game servers
(`oe serve-game`, 7778 = Channel 1, 7779 = Channel 2). Accounts and keys are kept in
`build\server-data`. Start as many players as you like, at any time, with `oe run`: click **Login**.
The game logs in as a guest, shows its session ticket to the channel server, is sent to the emptiest
channel (`oe run games/OnlineRPG --set channel=2` asks for one) and enters that world (seven players
per channel). Close a game server's window and its players see "The connection to the server was
lost."; OK closes the game. Close the channel server and the worlds keep running, but nobody new
gets in until it is back.

**Play offline** on the title screen is a world of one with the same rules. In the editor, Players 2 +
Play previews two peers in one process (no back end); `oe serve-game games/OnlineRPG` alone is a
server that lets everybody in. For other machines: bind the servers to reachable addresses
(`--bind`, `network.bind` in `project.json`), give each game server `--set address=<its address>`,
and start players with `--set login=host:port --set lobby=host:port`. Nothing is encrypted yet: use
it on a LAN.

## Files

| File | Runs on | Does |
|---|---|---|
| `scripts/connect.lua` | every peer | title screen, login server -> channel server -> game server, the notices |
| `scripts/game.lua` | game servers (or offline) | item spawns, pick-up, bags, drop and chat requests |
| `scripts/player.lua` | server + the owning client (prediction) | walking from the declared network inputs; the replicated state and name `hud.lua` shows |
| `scripts/hud.lua` | every peer | walk cycles, name plates, speech bubbles, chat log, bag panel, item bobbing |
| `scripts/camera.lua` | every peer | follow camera of the local player |
| `scripts/chatbox.lua`, `slot.lua`, `button.lua` | every peer | chat field, bag buttons, buttons that call another script |
| `scripts/lib/gate.lua` | game servers | the link to the channel server: register, population, "whose ticket is this?" -> `net.admit` or `net.kick` |
| `scripts/lib/platform.lua` | — | message numbers of the back end, where its servers are (launch parameters) |
| `scripts/lib/session.lua` | — | `authority()`, `send`/`on` that also work offline |
| `scripts/lib/items.lua`, `text.lua` | — | item kinds and limits; English/Korean lines of the connection screens |
| `scripts/lib/world.lua` | — | walkable area, spawn points, starting items (generated) |

Messages inside a world: clients send `chat(text)` and `drop(slot)` to
their game server; it sends `say(player, name, text)` to everybody and `inv(bag)` / `notice(text)` to
one player. Picking up is the declared action `E` in the input stream, so it needs no message.

`python tools/make_scene.py` writes the scene, materials, prefabs and `scripts/lib/world.lua` (edit the
script, not the scene); `python tools/make_art.py` writes the item art. The world textures are copies of
`samples/HD2D`, the font is Jua (SIL OFL, `assets/fonts/OFL-Jua.txt`).

## Back end

`server/` holds the game's own servers, separate programs that are neither engine nor game data
(`oe package` leaves the folder out): `oe_login` and `oe_channel`, built by `server\build.bat` next
to `oe.exe`. Design, protocol and work log:
[docs/ONLINERPG_SERVERS.md](../../docs/ONLINERPG_SERVERS.md).

Tests: `OnlineRPGGame` in `tests/tests.cpp` (offline and a loopback client),
`build\bin\oe_server_tests.exe` and `python games/OnlineRPG/server/test/login_test.py` (the back end
alone), `python tests/online_rpg_test.py` (the real back end, two game servers and client processes
over TCP, on free ports).
