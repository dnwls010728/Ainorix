# Networking — design guidelines

Purpose: give the engine the building blocks to make **network games** (small co-op/versus
sessions up to dedicated servers with many players) **later**, while a **single-player game stays
exactly what it is today**. This file is the contract for everyone (human or agent) who implements
networking: read it together with docs/DESIGN.md before touching `engine/net/`.

Status: **M1–M7 and M8a/M8b implemented; M8c Android verification remains pending** (M8 stays unchecked until then). Opt-in lobbies, RPC, lockstep and native rollback are available.
Authoritative replication, relevance, interpolation and owned prediction are available.
The milestone checklist, the implementation notes of every milestone (wire formats, bounds, recording
contracts, limits) and the platform verification record are in [NETWORK_NOTES.md](NETWORK_NOTES.md).

## 1. Goals and non-goals

Goals
- One engine, both kinds of games: single-player ignores networking; a network game turns it on
  with project data, not an engine fork.
- Support small sessions (2–8 players, listen host or peer-to-peer) **and** dedicated servers
  (tens to a few hundred players per process) with the same code base. Current limit: 64
  participants per session (63 remote on a dedicated server); see the audit follow-up in [NETWORK_NOTES.md](NETWORK_NOTES.md).
- Every capability is a command + Lua binding, testable headless and deterministically.

Non-goals (for now)
- Matchmaking/account/payment services, MMO-style sharding, voice chat.
- Massive single-process connection counts (10k+ TCP sockets). See §6 for how that stays possible.
- Anti-cheat beyond server authority (the server never trusts client state).

## 2. Rules (MUST / MUST NOT)

1. **Opt-in, zero cost.** Without a `network` section in `project.json` (or `mode: "none"`) the
   net module is not initialised: no sockets, no threads, no per-frame work, identical frame
   hashes. A test pins this (§10).
2. **One API (DESIGN.md §1.1).** Every feature is a `net.*` command (CLI/HTTP/MCP/editor) and, when
   gameplay needs it, a Lua binding. No network feature is editor-only or agent-only.
3. **Determinism is untouched.** Network data never brings wall-clock time, arrival order or thread
   timing into the simulation. It enters only at a fixed frame boundary in a defined order
   (player id, then sequence). `Engine::SimulateWorld` is shared by normal advancement and deterministic replay.
4. **Transport ≠ sync model.** What carries bytes (UDP/TCP/WebSocket/loopback) and how state is
   kept in sync (lockstep/rollback/authoritative) are independent layers, each replaceable.
5. **Portable C++17 outside `engine/platform/*`.** `engine/net/` has no OS headers; sockets live
   behind `Platform.h`. No new third-party dependency; framing, reliability, serialization and the
   handshake are written in-tree and unit-tested (like `core/Zip.cpp`).
6. **Never trust the network.** All incoming bytes are validated (length, counts, ids, enums,
   rate); malformed input is dropped and counted, never thrown out of the net layer, never
   allocates unbounded memory. Server-side handlers re-validate every client request.
7. **Safe by default.** The HTTP/MCP API stays on 127.0.0.1. A game server binds a non-loopback
   address only when `network.bind` says so. No secrets in logs; stdout stays JSON-only.
8. **Game code is mode-agnostic.** The same Lua scripts must run in single-player and in a
   network game (§8). Do not add APIs that only work with a server.
9. **No blocking in the simulation thread.** Sockets are non-blocking; the net layer is polled once
   per frame (or run on a worker that hands results over through a queue drained at the frame
   boundary).

## 3. Choosing a mode (project data)

```json
"network": {
  "mode": "none | lockstep | rollback | authoritative",
  "tickRate": 60,
  "maxPlayers": 4,
  "transport": "udp | tcp | websocket | loopback",
  "controlTransport": "tcp",
  "inputDelay": 2,
  "rollbackFrames": 8,
  "snapshotRate": 20,
  "interpolationFrames": 6,
  "predictionFrames": 32,
  "playerPrefab": "prefabs/player.prefab.json",
  "lateJoin": false,
  "admission": "auto | script",
  "timeoutSeconds": 30,
  "bind": "127.0.0.1",
  "port": 7778
}
```

| mode | Best for | Sync | Bandwidth | Needs |
|---|---|---|---|---|
| `none` (default) | single player | — | 0 | — |
| `lockstep` | RTS, co-op, turn-based, small sessions | all peers simulate; only **inputs** are exchanged; frame N runs when all inputs for N arrived | tiny | determinism (have), desync hash check |
| `rollback` | fighting/action versus, 2–4 players | like lockstep, but predicts missing inputs and re-simulates on mismatch | tiny | fast `SaveState/LoadState` |
| `authoritative` | FPS, many players, dedicated server | server simulates; clients send inputs, receive **state snapshots**, interpolate, predict own player | scales with changed state | replication rules |

`lateJoin` (authoritative only): the host keeps accepting players while the match runs, and a dedicated server is a
persistent world that starts without players and survives being empty (see NETWORK_NOTES.md, audit follow-up).

`timeoutSeconds` (5..120, default 30): a peer that sends nothing, or leaves a reliable message unacknowledged, for
this long is dropped. It used to be a fixed 10 s, which a client lost to any stall longer than that (a window held
in the OS move loop before `Window::SetModalFrame`, a loading hitch, a debugger break). A server that vanishes is
also noticed only after this time when the transport reports no close (UDP).

Network watchdog (`Engine::EnableNetworkWatchdog`, on in `oe run` and the packaged desktop/Android player, off in
tools and tests): when the main loop has not advanced the session for 0.1 s, a background thread advances only the
session's I/O at 60 Hz (heartbeats, acknowledgements, retransmits, buffered messages) until the loop is back. It
never touches the scene or the simulation and runs only while no engine entry point (`Tick`, `Step`, `Call`) holds
the engine lock, so it covers stalls outside the engine (GPU/driver hitches, OS calls) but not a long scene load
inside a frame. Test: `NetworkWatchdogKeepsStalledClient` (real UDP, 5 s timeout, a 6 s stall: dropped without it,
kept with it). `net.stats.watchdogTicks` counts its ticks.

`admission` (with `lateJoin`): `auto` admits every ready lobby player; with `script` a player waits in the lobby
until the server's Lua called `net.admit(id)`, so the game can verify it first. A server's scripts may also call
`net.kick`. The engine stops there: accounts, tickets, channel lists and persistence are services of the game,
reached with `link` (framed TCP from Lua, SCRIPTING.md) and told apart per process with launch parameters
(`oe serve-game ... --set channel=2`, `game.arg`). A project's `server/` folder holds such a back end and
is never packaged.

Pick by game: lockstep is the cheapest and fits the engine best; rollback needs the snapshot
work; authoritative is the way for many players and cheat resistance. All use the same session,
channel and transport layers and the same fixed-step simulation.

## 4. Architecture

```
Lua / commands (net.*)       gameplay API: RPC, events, players, stats
Sync model                   lockstep | rollback | authoritative        engine/net/sync/
Session                      host/join, lobby, player ids, ready, drop/reconnect, session seed
Channels                     reliable-ordered | reliable-unordered | unreliable | sequenced
Packet layer                 framing, fragmentation, acks, RTT/jitter/loss stats, MTU
Transport (ITransport)       UDP | TCP | WebSocket | Loopback (simulated network)
Platform sockets             engine/platform/*   Win32/POSIX UDP+TCP, Emscripten WebSocket
```

- `engine/net/` is a new module in the `OwnEngine` library; it never depends on render, audio or
  editor code. It talks to the engine through a narrow interface (`INetHost`: current frame, input
  injection, snapshot access) so it is testable without a full `Engine`.
- `ITransport` is the only place with `Send(peer, bytes)` / `Poll(events)`. Everything above it is
  transport-agnostic.
- `LoopbackTransport` is a deterministic in-memory network (seeded RNG: latency, jitter, loss,
  duplication, reordering). It powers tests, `net.simulate` and the editor's "Play with N players".

## 5. TCP, UDP and WebSocket — what each is for

The channel layer is identical on all transports; the transport defines what "unreliable" means.

| Transport | Use it for | Notes |
|---|---|---|
| **TCP** | login/lobby/chat/RPC, turn-based, **lockstep**, firewalled networks | reliable + ordered; head-of-line blocking on loss. Unreliable/sequenced channels are emulated by dropping stale messages on arrival (sequence numbers). Set `TCP_NODELAY`; length-prefixed frames with a hard maximum size; never block the sim thread. |
| **UDP** | **rollback**, **authoritative** action games | packet layer adds acks, retransmission for reliable channels, real unreliable delivery, fragmentation, congestion-safe send rate. Needs a handshake with a random cookie to avoid spoofed/amplified traffic. |
| **WebSocket** | browser builds | TCP semantics only (browsers have no UDP). Same channel interface; a WebRTC data-channel transport can be added later without touching upper layers. |
| **Loopback** | tests, editor, `net.simulate` | in-process, deterministic, no OS sockets. |

A session may use TCP for control (join, lobby, RPC) and UDP for game traffic
(`transport: "udp"`, `controlTransport: "tcp"`), and falls back to TCP when UDP cannot be
established.

## 6. I/O and threading model

- **Default: non-blocking sockets polled once per tick** (`poll`/`WSAPoll`). A game server is a
  fixed-step loop: read everything available → simulate → send. This keeps the net layer
  single-threaded and the simulation deterministic. UDP needs one socket for all clients; TCP needs
  one per client, which `poll` handles comfortably for the session sizes in §1.
- **IOCP / epoll / io_uring are not needed now.** They matter for very many concurrent TCP
  connections in one process. `ITransport` hides the readiness mechanism, so a faster backend
  (IOCP on Windows, epoll on Linux) can later replace the poll loop **inside
  `engine/platform/*`** without touching sessions, channels or sync. Add one only after measuring that
  connection count is the bottleneck.
- A worker thread is allowed only if it communicates through a queue that the main thread drains at
  the frame boundary (rule 9); simulation state is never touched from it.

## 7. Sync models

### 7.1 Lockstep / rollback
- Each local frame the net layer captures the local player's input (only the actions the game
  declares, as a compact bitfield + axes), tags it with `player` and `frame + inputDelay`, sends it,
  and the sim consumes the **merged inputs of all players for frame N** (`input.player(id)` in Lua;
  plain `input.*` is the local player).
- **Lockstep** runs frame N only when all inputs for N are present; otherwise it stalls (the game
  can show "waiting"). Drop policy (configurable): kick, or substitute empty input after a timeout.
- **Desync detection:** every K frames peers exchange the world hash (`Fnv1a64`, the frame hash
  machinery that already exists). A mismatch raises `net.desync`; `net.desync_report` dumps both
  scene JSONs for diffing.
- **Rollback** uses `Engine::SaveState()/LoadState()` semantics for scene, Jolt/Box2D, Lua,
  audio and RNG. Native snapshots retain Lua closure/upvalue/coroutine allocations, deep-copy
  runtime component pools, save Jolt solver/character state and capture Box2D world allocations.
  Correction restores the earliest affected checkpoint and replays at most `rollbackFrames`
  (1..8), independent of match history. Standalone snapshots include captured PCM; rollback
  checkpoints only retain mixer state and unconfirmed output.
- The host picks the session seed and sends it in the handshake; Lua `math.random` is seeded from it.

### 7.2 Authoritative server
- The server runs the full simulation using client inputs tagged with the client's frame.
- **Replication:** a `NetSync` component marks an entity as replicated; fields are chosen from the
  component schema (reflection already enumerates them) with per-field options (`always`,
  `onChange`, `ownerOnly`, `quantize`). Snapshots are delta-compressed against the last acked one
  and sent at `snapshotRate`; relevance filtering (distance/team) limits what each client gets.
- Network ids are assigned by the server; clients map `netId → local entity`. Spawn/despawn are
  reliable events and reference prefabs by path so clients instantiate locally.
- Clients interpolate remote entities (~100 ms buffer) and **predict** their own player by running
  the same simulation on their own inputs, reconciling against server snapshots.
- Ownership: `NetSync.owner` is a player id or the server; only the owner's inputs affect that
  entity, and the server validates everything (no client authority over game state).

## 8. Surface for games

Components: `NetSync` (replication settings), `NetPlayer` (entity controlled by a player id,
spawned by the session when a player joins).

Commands (each `Register(...)`ed with a `Params()` schema, `mutates` only if it edits the scene;
errors are `ApiError(code, message, hint)`): `net.state`, `net.host`, `net.join`, `net.leave`,
`net.players`, `net.kick`, `net.stats` (rtt, loss, bytes, frame lag), `net.simulate`
(latency/loss/jitter, seeded), `net.spawn_local_peers {count}`, `net.desync_report`.

Lua (documented in `docs/SCRIPTING.md` when added): `net.isHost()`, `net.isServer()`,
`net.isClient()`, `net.localPlayer()`, `net.players()`, `net.rpc(target, name, ...)` (targets:
`server`, `owner`, `all`, `others`, player id), `net.on(name, fn)`, `net.sender()`, `net.entities()`,
`input.player(id)`, callbacks `onPlayerJoined(id)`, `onPlayerLeft(id)`, `onNetState(state)`.
A raw `net.send`/message handler was planned here but is not implemented: bounded RPC covers it.

**Single-player compatibility:** with `mode: "none"`, `net.isServer()` is `true`,
`net.localPlayer()` is `1`, `net.players()` is `{1}` and `net.rpc` runs the handler locally. A game
written against this API runs unchanged as a single-player game or a network game.

Topologies (same code, different roles): **listen host** (a player is also the server),
**dedicated server** (`oe serve-game <project>` / player exe `--server`: headless fixed-step loop using the native networking platform,
no window, GPU or speaker device), **peer-to-peer** (lockstep/rollback; one peer coordinates join/lobby),
and an optional **relay/lobby** server later for NAT traversal and room codes.

Editor/tooling: a *Players: 1..N* dropdown for Play (N in-process peers over loopback, latency
slider) and a Network panel (players, stats, desync). `oe package` ships networking only when the
project enables it; the web build uses WebSocket and needs `oe serve-game` as its server (static
hosting cannot accept connections).

## 9. Security and robustness checklist (applies to every milestone)

- Hard limits: max packet/message size, max fragments, max reliable queue, max players, per-peer
  rate limit; exceeding them drops/kicks, never allocates.
- Bounds-checked reader/writer is the only way to parse bytes (no raw casts, no unchecked lengths).
- UDP handshake uses a random cookie before allocating peer state; sequence numbers + ack windows
  reject replays/duplicates.
- No pointers, no `std::hash` order, no uninitialised bytes on the wire; fixed endianness; explicit
  protocol version in the handshake with a clear error + hint on mismatch.
- Connection timeouts and graceful leave/kick; a dead peer must never stall the server loop.
- Encryption/authentication is out of scope for v1 (document it); the wire format leaves room for it.

## 10. Testing (each milestone adds tests to `tests/tests.cpp`)

- **Zero-cost pin:** a project without `network` produces identical frame hashes and starts no
  sockets/threads.
- Reader/writer: truncated, oversized and fuzzed input never crash or over-allocate.
- Packet layer/channels: fragmentation, ack windows, retransmission under seeded loss/reorder.
- Lockstep: 2 and 4 in-process peers, scripted inputs, 10 000 frames, random latency/loss — frame
  hashes equal at every checked frame. Same test over loopback and real localhost TCP/UDP sockets.
- Rollback: forced misprediction; after re-simulation the hash equals a never-predicted run.
- Authoritative: server + clients converge; prediction error bounded; spawn/despawn order stable.
- Acceptance: the engine tests run lockstep, rollback and authoritative matches over loopback and real sockets
  (`Network*` tests in `tests/tests.cpp`).
- Everything also runs as WebAssembly where applicable (`node build-web/bin/oe_tests.js`).

## 11. Status and details

The work log and the notes written while each milestone was built live in
[NETWORK_NOTES.md](NETWORK_NOTES.md), so this file stays the short contract. Look there for:

| Topic | Section of NETWORK_NOTES.md |
|---|---|
| Milestone checklist (what is done, what is unverified) | Implementation status (work log) |
| Transports, byte and packet codecs, channels | M1, M2, M3 implementation notes |
| Lobbies, players, readiness, RPC, session protocol | M4 implementation notes |
| Lockstep, rollback, `sim.record_state` / `sim.save_state` recording contract and bounds | M5a/M5b, M5c |
| Authoritative replication: `NetSync`/`NetPlayer`, snapshots, relevance, interpolation, owned prediction, limits | M6 |
| Dedicated server, WebSocket, editor previews, platform verification | M7, M8a/M8b |
| Later corrections | Audit follow-up |

Using the feature from a game: §8 above and [SCRIPTING.md](SCRIPTING.md) (Lua `net.*`).
