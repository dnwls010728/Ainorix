# OnlineRPG — server architecture (design)

How `games/OnlineRPG` becomes an online game with three kinds of servers: a **login server**, a
**channel server** and **game servers**. Chosen for performance first; where a choice does not
affect performance, for the least code. This replaces the M2 arrangement in which the login server
was the game project started on another port ([ONLINERPG.md](ONLINERPG.md)). Status: **design, nothing
built yet**; the work log is the last section.

## 1. Decisions

| Question | Decision | Why |
|---|---|---|
| Language of login/channel servers | C++17 | Native, no GC pauses; shares the engine's sockets, byte codecs and binary Json, so each protocol is implemented once |
| Where the code lives | `games/OnlineRPG/server/` with its own CMake build | Used by this project only; the engine build and `third_party/` do not change |
| Wire format everywhere | Length-prefixed binary frames over TCP | No text parsing, one framing for every link, persistent connections where push is needed |
| Game server | Engine dedicated server (`oe serve-game`), one per (channel, map) | The simulation must be the same code as the client's prediction |
| Identity between servers | Tickets signed with HMAC-SHA256 | Verified locally with a shared key: no database or server round trip per check |
| Storage | SQLite (WAL) in `server/vendor/sqlite/`, one writer thread, write-behind | Fastest option on one machine; behind a `Store` interface so a server database can replace it |
| Engine | Stays a generic dedicated-server engine | Three small generic additions (§7); it never learns what a login or a channel is |

## 2. Overview

```
                       ① TCP, short                     accounts.db
   client ───────────────────────────────▶ login server ────────▶ SQLite
     │
     │                 ② TCP, while choosing            characters.db
     ├───────────────────────────────────▶ channel server ──────▶ SQLite
     │                                          ▲
     │                                          │ ④ TCP, permanent (register, load, admit,
     │                 ③ engine session         │    save, kick, transfer, broadcast)
     └───────────────────────────────────▶ game server × N   (oe serve-game; channel × map)
```

| Server | Program | Talks to | Keeps |
|---|---|---|---|
| Login | `oe_login` | clients (①) | accounts, password hashes |
| Channel | `oe_channel` | clients (②), game servers (④) | live server registry, who is online where, characters |
| Game | `oe serve-game games/OnlineRPG` | clients (③), channel server (④) | the live world of one map; nothing durable |

The login and channel servers never talk to each other: the session token the login server issues
is signed, and the channel server verifies the signature with the same key. Game servers never talk
to each other: the channel server is the hub.

## 3. Capacity model

- An engine server process holds 63 players (NETWORK_NOTES.md). **Map** = one game server process;
  **channel** = one full set of maps. Capacity = 63 × maps × channels, grown by starting processes.
- The channel server sends a player to the map its character is on, in the chosen channel; a full
  map is refused and the same map of another channel offered.
- More than 63 players on one map is engine work (peer tables, per-client snapshot cost, the 1024
  entity / 56000 byte snapshot caps) plus measurements, and is outside this design. Inside a map,
  `NetSync.distance` (relevance) is the lever that keeps snapshot cost from growing with the square
  of the player count; maps are authored with it from the start.

First milestone: one map (`town`), two channels, so two game server processes.

## 4. Wire protocol

One framing for links ①, ② and ④:

```
u32 length (of what follows, ≤ 65536)   u16 type   payload
```

- Little endian, read and written with the bounds-checked `Reader` / `Writer` of
  `server/common/Wire.h`; no raw casts, no unchecked lengths (NETWORK.md §9).
- Payloads of fixed shape (heartbeats, tickets, results) are plain fields: `u8`/`u16`/`u32` and
  `s1` strings (a `u8` length and UTF-8 bytes), which are `string.pack("<I2")`, `"<s1"` ... in the
  game's Lua. Payloads that carry game data (a character) use the engine's bounded binary Json
  (`engine/net/JsonCodec.h`: interned keys, varints, float32 when exact), so Lua tables on the game
  server need no hand-written schema.
- `u16 type` numbers and error codes are listed in `server/common/Protocol.h`, which the engine
  never includes: Lua sends and receives `(type, payload)` through the generic `link` (§7).
- Every link starts with `Hello {u16 protocol version, u8 role}` from the side that connected. It
  is not answered: the first request may follow in the same packet. A wrong version or role gets
  `Error {u16 code}` and a close.
- A server that closes after a last answer first stops sending and waits up to a second for the
  other side to close, so the answer is not lost to a connection reset.
- Limits per connection: 64 KiB frame, 256 KiB unsent queue (then the connection is closed), an idle
  timeout per link. Exceeding a limit closes the connection and is counted; it never allocates.

### 4.1 Tickets

A ticket is 30 bytes, sent as 40 base64url characters:

```
u8 kind (1 session, 2 entry)   u32 account   u16 server (0 for a session)   u32 expires (unix s)
u32 nonce   u8[15] HMAC-SHA256(key, all previous bytes) truncated
```

- **Session ticket** (login → client → channel server): valid 12 hours, proves the account.
- **Entry ticket** (channel → client → game server): valid 30 seconds, bound to one game server id,
  single use (the channel server remembers issued nonces until they expire).
- 40 printable characters fit the engine's session join name (≤ 64 bytes), which is how an entry
  ticket reaches the game server (`net.join {name = ticket}` → `net.name(id)`): no engine protocol change.
- A ticket carries no name. The login server signs the account's name separately (**name proof**:
  20 base64url characters of `HMAC-SHA256(key, 0x03, u32 account, name)`), the client presents
  name and proof with its session ticket, and the channel server passes the name on to the game
  server with the admission. The login and channel servers never talk to each other.
- Two keys, each 32 random bytes as 64 hexadecimal digits in a file outside the project, created by
  whichever server starts first: the **ticket key** (`--key-file`, login server and channel server
  only) and the **link key** (`--game-key-file`, channel server and game servers). A game server
  runs the game's scripts and talks to players, so it gets only the link key: it can register and
  ask, it cannot sign tickets.

## 5. Flows

### 5.1 Login (①)

```
client → login:   Hello, then  Login {s1 name, s1 password}  |  Guest {}  |  Register {s1 name, s1 password}
login → client:   LoginResult {u32 account, s1 name, s1 session ticket, s1 name proof}  |  Error {u16 code}
```

The client opens the connection, sends one request, gets one answer and closes. A dead or silent
login server is a connect error or a 10 s timeout on the client, which shows "could not reach the
server". Nothing else ever depends on the login server: players already in the game are unaffected
when it is down.

### 5.2 Choosing a channel (②)

```
client → channel:  Hello, Session {s1 session ticket, s1 name, s1 name proof}
channel → client:  Channels {u8 count, per channel: u8 channel, u16 players, u16 capacity, u8 online}   (again every 2 s)
client → channel:  Enter {u8 channel}         (0 = the emptiest that has room)
channel → client:  EnterResult {s1 address, u16 port, s1 entry ticket, u8 channel, s1 map}  |  Error {code}
client → channel:  Ping {}                    every 5 s while waiting (30 s of silence closes the link)
```

The connection stays open while the player looks at the list, so counts are live and a channel
server that dies is noticed at once. It closes after `EnterResult`. `Error` for a full or offline
channel leaves it open: the player may pick another one. A place is held for 30 seconds per entry
ticket, so a channel is not promised to more players than fit.

### 5.3 Entering a game server (③ + ④)

```
client:        net.join {address, port, name = entry ticket}
game server:   onPlayerJoined(id) → Admit {ticket, player id}                 → channel server
channel:       verify signature, expiry, server id, nonce unused, account not online elsewhere
               → AdmitResult {u32 player, u16 code, u32 account, s1 name}     → game server   (S4 adds the character)
game server:   net.admit(id)  → the avatar spawns from the character; rpc "welcome"
               (failure or 5 s without an answer: net.kick(id))
```

The player is held in the lobby until the answer arrives (`network.admission: "script"`, §7), so an
unverified client never has an avatar. The avatar's name is the one from `AdmitResult`, written by
the server into the replicated `Tag`; the owner's prediction keeps what the server wrote.

### 5.4 Game server ↔ channel server (④)

```
game → channel   Hello {u16 version, u8 role, s1 link key}
                 GameRegister {u8 channel, s1 map, s1 address, u16 port, u16 capacity}   → GameRegistered {u16 server id}
                 Heartbeat {u16 players}                             every second
                 Admit {u32 player, s1 ticket}                       → AdmitResult
                 Save {account, version, character}                  on leave, on change (≥ 5 s apart), every 60 s
                 Left {account}
                 Transfer {account, map | channel}                   → TransferResult {address, port, entry ticket}
channel → game   Kick {account, reason}                              duplicate login, ban
                 Broadcast {kind, text}                              notices, later global chat
```

The game server connects when it starts and reconnects with backoff (1, 2, 4, 8, 10 s) if the link
drops. A server that registers for a channel and map that is taken replaces the old registration
(a restart whose old link has not timed out) and gets a new id, which makes entry tickets issued
for the old one useless. While the link is down it keeps simulating, admits nobody, and queues
saves (bounded; the newest save per account wins). The channel server treats a dropped link, or 5 s without a heartbeat, as the server
being offline: its players are free to enter elsewhere, with their last saved character.

### 5.5 Changing map or channel

The game server saves the character, asks `Transfer`, and sends the result to the client by RPC; the
client leaves and joins the new address with the new ticket (§5.3). The client never asks the
channel server for a transfer, so it cannot go where the game server did not decide.

### 5.6 Rules

- One live session per account: `Enter` for an account that is online sends `Kick` to the game
  server holding it, and answers after that server confirmed `Left` (or after 3 s).
- A save is accepted only with a `version` newer than the stored one, so a late save from a dying
  server cannot overwrite a newer state.
- Names 2..16 characters, passwords 8..128 bytes; per-address rate limit on the login server
  (5 attempts per minute) applied before any hashing.

## 6. Server internals (performance)

Both servers are the same skeleton (`server/common/`):

```
I/O thread ── accepts, reads, frames, validates, writes; never blocks
   │  bounded queues
   ├─ worker pool (login server: one per core) ── password hashing only
   └─ store thread ── the only one that touches SQLite; prepared statements; batches writes
```

- **I/O** (`common/Net.h`): the servers' own non-blocking sockets, one thread, readiness by `poll`
  (`WSAPoll`). That holds for the connection counts here (channel: tens of game servers and the
  players currently choosing; login: connections that live under a second). `Poller` is an
  interface so an IOCP / epoll backend can replace `poll` **after a measurement shows it is the
  limit** (NETWORK.md §6); nothing above it changes. The engine's `NetSocket` is not used: it hides
  the native handle a poller needs, and the engine should not grow for a back end.
- **No allocation per frame on the hot path**: one contiguous receive buffer per connection that
  the socket reads into and frames are parsed out of in place (`FrameBuffer`; handlers get a view),
  one send buffer written at once when the socket takes it; both keep their capacity across
  connections. `Heartbeat` and `Admit` are handled without touching the store.
- **Login cost is the password hash by design**: PBKDF2-HMAC-SHA256 with 100 000 rounds, measured
  33 ms per attempt on one core of the development PC (the inner loop is two compression calls per
  round), so throughput ≈ 30 logins per second per core. It runs on the worker pool, the queue is
  bounded (overflow answers "busy"), and the rate limit rejects before hashing. Guests skip the hash.
- **Channel server state is memory**: the server registry, account → server map, used nonces and a
  cache of the characters of online accounts. `Admit` reads the cache (loaded when `Enter` was
  answered), so entering a world costs no disk access on the path the game server waits on.
- **Write-behind**: `Save` updates the cache and marks it dirty; the store thread writes dirty
  characters in one transaction every 2 s and when an account leaves. A crash of the channel server
  loses at most those 2 s; a crash of a game server loses at most what it had not sent (≤ 60 s of
  position, nothing of the bag, which is saved on change).
- **SQLite**: WAL mode, `synchronous = NORMAL`, one connection, prepared statements, an index on
  account name. One file per server (`accounts.db`, `characters.db`), so the two servers can run on
  different machines.
- **Crypto in-tree** (`server/common/Crypto.cpp`): SHA-256, HMAC, PBKDF2, constant-time compare;
  checked against the published test vectors (RFC 4231, RFC 6070 style vectors for SHA-256).
  Random bytes from the platform (`PlatformRandomBytes`, already used for session cookies).

Targets to verify with the load tool (S6), not claims: login server 1 000 concurrent connections and
100 guest logins per second; channel server 200 game servers, 2 000 choosing clients, 5 000 `Admit`
per second; no frame of a game server delayed by link ④ (it is polled once per tick, never blocking).

## 7. What the engine needs (generic)

1. **`link`** — outbound framed TCP for Lua, on clients and servers:
   `link.open {address, port}` → id; `link.send(id, type, payload)`; `link.poll(id)` → messages and
   state (`connecting`, `open`, `closed` + reason); `link.close(id)`. Non-blocking, polled at the frame
   boundary. A payload is a string of bytes (fixed layouts: `string.pack` / `string.unpack`) or any
   other Lua value sent as bounded binary Json; received payloads are bytes, `link.decode` turns
   binary Json back into a value. Bounds: 8 links, 64 KiB frames, 256 KiB queued, 512 unread.
   Links close when the Lua state is reset, so a client's link ② ends by itself when it enters the
   world; a predicting client cannot open one, a lockstep/rollback match has none.
   Commands `link.*` mirror it for tools and tests.
2. **Launch parameters**: a game server is `oe serve-game ... --port 7778 --set channel=1 --set
   map=town --set platform=<channel server>:7003 --set address=<own public address> --set-file
   linkkey=<path>`; a client may get `--set login=host:port --set lobby=host:port` (defaults
   127.0.0.1:7001 / 7002 in `scripts/lib/platform.lua`) and `--set channel=N` (until there is a list
   on screen). Read with `game.arg(name)`; they survive the match start (unlike `game.set` data). `--set-file`
   passes a file's text, so the key is neither on the command line nor in the project (Lua has no
   file access).
3. **Script-controlled admission**: `network.admission: "script"` + Lua `net.admit(id)`. With it a
   ready lobby player waits until the game calls `net.admit`; default stays automatic. The
   server's scripts may call `net.kick` (refused ticket, duplicate login).
4. **Packaging**: a project's `server/` folder is not game data (like `tools/` and `web/`), so server
   sources and SQLite never ship in a client build.

Already there and used as is: `network.lateJoin`, `net.name`, `net.kick`, `onPlayerJoined/Left`, RPC,
local UI input in a match, `game.quit`, scripts that keep running after a stopped match.

## 8. Project layout

```
games/OnlineRPG/
  server/
    CMakeLists.txt, build.bat      builds oe_login, oe_channel, oe_loadtest → build/bin/
    common/     Net (FrameBuffer, Poller, FrameServer), Wire (Reader/Writer, names), Protocol.h,
                Ticket, Crypto, Store (SQLite wrapper), Workers (TaskQueue), Log.h
    login/      main.cpp, LoginServer
    channel/    main.cpp, ChannelServer (registry, entry tickets, both links); Characters comes with S4
    loadtest/   main.cpp (simulated clients and game servers)
    vendor/sqlite/   sqlite3.c, sqlite3.h (amalgamation, unmodified, version noted in README)
    test/       main.cpp: oe_server_tests (crypto vectors, tickets, frame fuzz, store, FrameServer and
                LoginServer over real sockets); login_test.py: oe_login as a process
  scripts/
    connect.lua         title → login (①) → channel server (②) → net.join with the ticket (③)
    game.lua            world rules
    lib/gate.lua        a game server's link to the channel server (④): register, heartbeat, Admit, Left
    lib/platform.lua    message numbers, endpoints from launch parameters, Hello
  tools/start_servers.bat   oe_login, oe_channel, then one oe serve-game per (channel, map)
```

The servers are their own CMake project and do not link the engine. The channel server will compile
`engine/core/Json.cpp` and `engine/net/JsonCodec.cpp` into itself for characters (they depend on
nothing else); nothing from scene, render, script, physics or platform is used.

Removed from the M2 project: the login role in `game.lua`, `scripts/lib/servers.lua`, the `enter` RPC.

## 9. Failure handling

| What fails | Who notices | What happens |
|---|---|---|
| Login server | client: connect error / 10 s timeout | "could not reach the server"; players in game unaffected |
| Channel server | choosing clients: link closes; game servers: link ④ closes | clients see the popup; game servers keep running, queue saves, reconnect; nobody new can enter |
| Game server | its players: engine session ends; channel server: link ④ closes | popup with **Reconnect** (back to §5.2) and Quit; characters as last saved |
| Client vanishes while choosing | channel server: link closes | nothing to clean up |
| Client vanishes in game | game server: session timeout | avatar removed, `Save`, `Left` |

## 10. Security notes

- **No TLS yet.** Links ① and ② are plain TCP, so a password can be read on the network path.
  Until the client link gets TLS (Schannel on Windows behind `Platform.h`, or a vendored TLS in
  `server/vendor/` plus the same on the client), run this on a LAN or for development only. The frame
  layer is unchanged by TLS; it is the first item after the milestones below.
- Link ④ binds to an internal address and requires the link key in its `Hello`; a game server is
  trusted with the characters of the players it holds, nothing else (it never has the ticket key).
- An entry ticket is the player's session join name, and the engine tells lobby names to the
  other peers of that game server. That is harmless here: the ticket is bound to this server, the
  server asks about it the moment the player connects, and it works once.
- Every length, count, id and enum from the network is validated before use; a bad frame closes the
  connection. `test/` fuzzes the frame parser and the ticket decoder.

## 11. Not in this design

Password reset and e-mail, payments, friends / guild / party (they fit as channel-server state plus
`Broadcast`), several channel-server instances (the registry and online map would move to a shared
store), more than 63 players on one map, anti-cheat beyond server authority.

## Implementation status (work log)

- [x] S1 — engine: `link.*` (Lua + commands), `--set` / `--set-file` / `game.arg`, `network.admission` +
      `net.admit`, `net.kick` from a server's scripts, `server/` excluded from packages (§7; details in
      NETWORK_NOTES.md, audit follow-up). Verified on Windows: `oe_tests` 198 tests
      (`LinksAndLaunchArgs` against a real TCP listener, `NetworkScriptAdmission`), `oe exec ...
      game.args --set channel=2` from the CLI. Not verified: `oe serve-game --set` with a real
      service (S3), the packaged player's flags. The game still runs the M2 arrangement until S3.
- [x] S2 — `server/common` (Net, Wire, Crypto, Ticket, Store, Workers) and `oe_login`: Register,
      Login, Guest, per-address rate limits, store thread + hashing pool, key file created on first
      start. SQLite 3.50.4 in `server/vendor/sqlite/`. Build: `games\OnlineRPG\server\build.bat` →
      `build/bin/oe_login.exe`, `oe_server_tests.exe`. Verified on Windows: `oe_server_tests`
      (11 tests, 23 236 checks: SHA-256 / RFC 4231 / PBKDF2 vectors, ticket forgery, frame fuzz,
      store, FrameServer limits and LoginServer over real sockets incl. 24 concurrent clients);
      `python games/OnlineRPG/server/test/login_test.py` (the process against Python's `hmac` and
      `hashlib.pbkdf2_hmac`, malformed input, restart, rate limit, and the engine logging in
      through Lua `link` + `string.pack`). Measured, one sequential client: login 34 ms,
      guest 1.8 ms. Not verified: Linux/macOS build (the POSIX branches are written, never
      compiled), load (S6), a sanitizer run of the frame fuzz. Defaults: port 7001, `accounts.db`
      and `server.key` in the working directory.
- [x] S3 — `oe_channel` (players on 7002, game servers on 7003; `FrameServer` got a second listener)
      and the game on the new flow: `connect.lua` (login → channel server → `net.join` with the
      entry ticket), `lib/gate.lua` (register, heartbeat, Admit → `net.admit` / `net.kick`, reconnect),
      `lib/platform.lua`; `network.admission: "script"` in `project.json`; the M2 login role and
      `lib/servers.lua` are gone; `tools/start_servers.bat` starts all five programs. Added to the
      design while building: the name proof, the separate link key, `Ping`, the held place per
      entry ticket, a registration replacing an older one (§4.1, §5). Verified on Windows:
      `oe_server_tests` (12 tests, 23 274 checks; `ChannelServer` with fake players and game
      servers over real sockets), `python tests/online_rpg_test.py` (five real processes plus
      clients: enter a running world, pick up / chat / drop, forged, reused and misdirected tickets
      kicked without an avatar, a game server dying → popup + quit and the next login on the other
      channel, an offline channel → notice, the channel server dying → worlds keep running, login
      fails with a notice → it returns and the game server registers again), `start_servers.bat`
      run for real (entry tickets for both channels on the default ports), `oe_tests` 198.
      Not verified: windows played by hand, two machines (`--set address`, non-loopback binds).
      Until S5 the client logs in as a guest and takes the emptiest channel (or `--set channel=N`);
      the popup after a lost world still only quits.
- [ ] S4 — characters: load at Enter, Save / Left, write-behind, version check; duplicate-login Kick.
- [ ] S5 — client UI: name / password fields, channel list with live counts, Reconnect on the popup.
- [ ] S6 — `oe_loadtest` and the measurements of §6; fix what they show; record the numbers here.
- [ ] S7 — a second map and Transfer.
- [ ] Open: TLS for links ① and ②; Android runtimes rebuilt for S1 (the web runtime was, 2026-10-05; a browser
      client still needs a WebSocket form of links ① and ②); IOCP / epoll backend if
      S6 shows `poll` is the limit.
