# Networking — design guidelines and work log

Purpose: give the engine the building blocks to make **network games** (small co-op/versus
sessions up to dedicated servers with many players) **later**, while a **single-player game stays
exactly what it is today**. This file is the contract for everyone (human or agent) who implements
networking: read it together with docs/DESIGN.md before touching `engine/net/`.

Status: **M1–M2 foundations implemented.** No session, sockets or gameplay integration yet.
Progress is tracked in §11.

## 1. Goals and non-goals

Goals
- One engine, both kinds of games: single-player ignores networking; a network game turns it on
  with project data, not an engine fork.
- Support small sessions (2–8 players, listen host or peer-to-peer) **and** dedicated servers
  (tens to a few hundred players per process) with the same code base.
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
   (player id, then sequence). `Engine::SimulateFrame` stays the single place the world advances.
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
- **Rollback** needs `Engine::SaveState()/LoadState()` — an in-memory snapshot of scene, Jolt and
  Box2D bodies, Lua state, audio events and RNG, fast enough to restore up to `rollbackFrames`
  frames per tick. Until it exists, `mode: "rollback"` is rejected with an `ApiError` + hint.
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
`server`, `owner`, `all`, `others`, player id), `net.on(name, fn)`, raw `net.send`/message handler,
`input.player(id)`, callbacks `onPlayerJoined(id)`, `onPlayerLeft(id)`, `onNetState(state)`.

**Single-player compatibility:** with `mode: "none"`, `net.isServer()` is `true`,
`net.localPlayer()` is `1`, `net.players()` is `{1}` and `net.rpc` runs the handler locally. A game
written against this API runs unchanged as a single-player game or a network game.

Topologies (same code, different roles): **listen host** (a player is also the server),
**dedicated server** (`oe serve-game <project>` / player exe `--server`: headless `null` platform,
no window or audio output), **peer-to-peer** (lockstep/rollback; one peer coordinates join/lobby),
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
- Samples are acceptance tests: networked Platformer (2-player co-op, lockstep) and FPS
  (authoritative).
- Everything also runs as WebAssembly where applicable (`node build-web/bin/oe_tests.js`).

## 11. Implementation status (work log)

Definition of done for every milestone: code + tests green + docs/API.md regenerated if commands
changed + this log ticked in the same commit (DESIGN.md §4–5).

- [x] M0 — design guidelines (this document)
- [x] M1 — `engine/net`: `ITransport`, `LoopbackTransport` (seeded latency/loss/reorder),
      bounds-checked byte reader/writer, zero-cost pin test
- [x] M2 — packet layer + channels (reliable/unreliable, fragmentation, acks, RTT/loss stats)
- [ ] M3 — platform UDP + TCP sockets (`Platform.h`: Win32 + POSIX), `UdpTransport`,
      `TcpTransport` (length-prefixed frames, NODELAY, UDP→TCP fallback), WebSocket transport for
      web (Emscripten); tests on loopback and real localhost sockets
- [ ] M4 — session layer (host/join/lobby/player ids/handshake/version/seed), `net.*` commands,
      Lua `net` basics (`isServer`, `localPlayer`, `rpc`, `on`), `mode: none` semantics
- [ ] M5 — lockstep (input merge, frame gating, desync hash, `input.player(id)`); then
      `Engine::SaveState/LoadState` and rollback
- [ ] M6 — authoritative: `NetSync`, snapshots/deltas, interpolation, prediction, relevance
- [ ] M7 — headless dedicated server (`oe serve-game`, `--server`), editor Players×N play +
      Network panel, `net.simulate`
- [ ] M8 — networked sample games, docs (`API.md`, `SCRIPTING.md`, `PLATFORMS.md`), web and Android
      verification, optional relay/lobby

Open questions / unverified:
- `SaveState` cost for Jolt/Box2D/Lua (decides how many rollback frames are affordable) and
  whether restoring Jolt/Box2D bodies is bit-exact (needs a determinism test).
- Web hosting of a WebSocket game server (needs `oe serve-game`).
- When (if ever) a platform backend such as IOCP/epoll is worth adding (§6): measure first.

### M1 implementation notes

- `net/Transport.h` defines a non-blocking datagram interface. Peer handles are not session
  player ids. Poll receives an explicit monotonic simulation frame; backwards polling is rejected.
  `Send` accepts at most 64 KiB and copies the payload. Connection lifecycle events belong to M3/M4.
- `net/LoopbackTransport.h` provides an explicitly constructed shared `LoopbackNetwork` and
  endpoints with unique nonzero ids. Poll each endpoint before sending at a frame boundary.
  Send uses the sender's last polled frame (initially zero). Repeat the same seed and call order
  to reproduce delivery. Arrival order is deadline, then insertion sequence; the session layer
  will impose player/sequence ordering before simulation.
- Fault settings use frames and integer permille: latency, signed jitter clamped at zero,
  loss, duplication and extra random delay for reordering. Each delay setting is limited to
  3600 frames; probabilities to 0..1000. Seed zero uses seed one. No wall time or OS RNG.
- Hard limits per wire: 64 endpoints, 1024 queued datagrams, 4 MiB queued payloads, 64 KiB per
  datagram. Invalid/full sends return false and increment the drop count before copying data;
  simulated loss returns true and increments that count. Duplicate sends reserve both copies
  together. Endpoint destruction discards pending traffic both to and from it.
- `net/Bytes.h` encodes unsigned 8/16/32/64-bit little-endian values and uint32-length blobs.
  Writers have an explicit capacity; readers require a blob limit before allocating. Failures
  are sticky and preserve output and cursor/buffer. No unchecked casts or wire structs.
- M1 adds no `Engine` member, initialization, tick hook, socket or worker. Network project
  configuration is not activated until later milestones. `NetworkInactiveZeroCost` compares
  scene/frame hashes with no network section and explicit `mode: none`, and pins network
  construction/poll counters during open, input, simulation, rendering and stop.
- Validation: Windows Release `build.bat`; all 92 `oe_tests` pass, including
  `NetworkBytesBoundsAndEndian`, `NetworkLoopbackDeliveryAndLimits`,
  `NetworkLoopbackSeededFaults`, `NetworkInactiveZeroCost` and `AgentInstructionsInSync`.
  CLI smoke check: `oe exec samples/Hello sim.step '{"frames":120}'` returned `ok: true`,
  frame 120 and zero script errors.
  No commands or component schemas changed, so `docs/API.md` is unchanged.
- [ ] Web/Android build and test validation: run `build_web.bat` plus Node tests and
  `build_android.bat` plus device tests when SDKs are available. M1 is portable library code
  with no player integration; committed player runtimes retain their existing behavior.
  The SDKs are unavailable in this environment; prebuilt runtimes were not rebuilt.

### M2 implementation notes

- `net/Packet.h`: protocol-v1 codec and 64-bit packet receipt window. A 52-byte header uses
  explicit little-endian fields, magic/version, kind/channel, zero reserved byte, packet sequence,
  ACK highest/mask, message sequence, fragment index/count/total and length-prefixed payload.
  Datagrams are at most 1200 bytes; messages at most 64 KiB, with at most 64 fragments. The codec
  checks exact fragment geometry, truncation, trailing bytes, enums and ACK masks before acceptance.
- `net/Channels.h`: four independent lanes (`ReliableOrdered`, `ReliableUnordered`,
  `Unreliable`, `Sequenced`). Ordered messages wait for gaps; unordered messages deliver once on
  completion; sequenced messages discard older completed inputs. Packet receipts report RTT/loss,
  while **message ACKs** release reliable sends only after complete reassembly. Whole-message
  retransmission makes expiry of incomplete assemblies safe even after individual packet ACKs.
- Timing uses explicit frames. Retransmission starts at 12 frames and adapts to RTT/jitter,
  capped at 120 frames. Incomplete assemblies expire before incoming processing after 120 idle frames.
  A reliable send still unacknowledged 600 frames after enqueue produces one terminal `TimedOut`
  event, releases buffers and rejects new sends until the peer is removed/re-registered, including
  when transport backpressure prevents the first packet. These timings are configurable. Queue age
  uses the last polled frame (initially zero); poll the current frame before new sends.
- Per peer: 64 outgoing messages, 4 MiB each of send/receive payloads, 64 assemblies (including
  completed ordered messages), 32-message reliable sliding window per lane, 256 sent-packet samples,
  64 queued message ACKs, 16 outgoing datagrams and 128 incoming datagrams per frame. Repeated
  same-frame polls do no work; budgets cannot be bypassed by extra calls. Only `AddPeer` allocates
  peer state. Unknown peers cannot allocate assemblies or cause responses.
- RTT/jitter use integer Q8 EWMA frame values (samples clamped to 36000 frames), keeping
  retransmission scheduling independent of floating-point rounding/contraction. Convert reported
  frame values to milliseconds using the session tick rate in M4.
  Loss is an estimate from expired/untracked data-attempt ACKs, including late acknowledgements;
  ACK-only datagrams are not counted as lost data. Retries use fresh packet sequences and stable
  message ids. ACK-only packets do not trigger ACK replies. Sequence zero is reserved; reconnect
  before uint64 sequence exhaustion. Session identity/authentication/reconnect replay isolation
  remain M4 responsibilities; peer registration is not a public-network handshake.
- No `Engine` hooks, commands or component schemas added; `docs/API.md` remains unchanged.
  Single-player construction/poll pin now covers both M1 loopback and M2 channel endpoints.
- [x] Windows Release `build.bat`: no new compiler warnings. Full `oe_tests`: 99 tests,
  zero failed checks. New coverage: `NetworkPacketCodecAndAckWindow`,
  `NetworkChannelOrderingAndReplay`, `NetworkChannelFragmentValidationAndExpiry`,
  `NetworkChannelsReliableUnderLoss`, `NetworkChannelQueueBudgetAndTimeout`,
  `NetworkChannelsAckLossAndAssemblyRecovery`, `NetworkChannelsUnreliableAndBackpressure`.
  This includes exact-once reliable delivery under 25% seeded loss/duplication/reordering,
  64 KiB fragmentation, ACK loss, ordered retention across incomplete-assembly expiry,
  progression beyond the 32-message reliable window, replay rejection, malformed/fuzzed
  headers, byte/count/rate limits, blocked sends and terminal timeouts. Repeat seeds produce
  identical delivery traces; different seeds change those traces. Inactive hashes/counters and
  `AgentInstructionsInSync` still pass. CLI `oe exec samples/Hello sim.step '{"frames":120}'`
  returned `ok: true`, frame 120 and zero script errors.
- [ ] Web/Android tests and prebuilt-runtime rebuild: `build_web.bat` reports Emscripten
  not found; `build_android.bat` reports Android NDK not found. No real sockets or devices
  tested in M2; committed runtimes were not rebuilt. Run the SDK commands noted above when
  available. Public-network handshake/authentication and congestion control remain later work.
