# Networking — design guidelines and work log

Purpose: give the engine the building blocks to make **network games** (small co-op/versus
sessions up to dedicated servers with many players) **later**, while a **single-player game stays
exactly what it is today**. This file is the contract for everyone (human or agent) who implements
networking: read it together with docs/DESIGN.md before touching `engine/net/`.

Status: **M1–M4 implemented.** Opt-in host/join lobbies, player ids/readiness, session-cookie
handshakes, commands and Lua RPC are available. Simulation synchronization remains M5/M6;
M4 sessions advance independent local worlds. Platform verification limits are recorded below.
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
- **Rollback** uses `Engine::SaveState()/LoadState()` semantics for scene, Jolt/Box2D, Lua,
  audio and RNG. M5 currently provides an exact reference backend that rebuilds these states by
  replaying their input journal. `mode: "rollback"` enables this experimental backend; it is
  bounded to 12000 frames and restores in O(match history), not O(rollbackFrames).
  Fast native snapshots that meet a real-time frame budget remain an unchecked M5 requirement.
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
- [x] M3 — platform UDP + TCP sockets (`Platform.h`: Win32 + POSIX), `UdpTransport`,
      `TcpTransport` (length-prefixed frames, NODELAY, UDP→TCP fallback), WebSocket transport for
      web (Emscripten); tests on loopback and real localhost sockets
- [x] M4 — session layer (host/join/lobby/player ids/handshake/version/seed), `net.*` commands,
      Lua `net` basics (`isServer`, `localPlayer`, `rpc`, `on`), `mode: none` semantics
- [ ] M5 — lockstep and full-state rollback
  - [x] M5a — input merge, frame gating, ready barrier, desync reports, `input.player(id)`
  - [x] M5b — exact reference `Engine::SaveState/LoadState` and prediction/correction by replay
  - [ ] M5c — fast native Lua/Jolt/Box2D snapshots; restore cost bounded by rollback window,
        benchmarked against the 1/60 s game budget (reference replay is not this backend)
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

### M3 implementation notes

- `platform/Network.h` (included by `Platform.h`) exposes RAII non-blocking UDP/TCP sockets,
  numeric IPv4 addresses and browser binary WebSockets. OS calls stay in
  `platform/net/NativeSockets.cpp` (WinSock/POSIX); browser callbacks in `platform/web/NetworkWeb.cpp`.
  Bind defaults to loopback; other addresses require explicit caller data. No DNS, socket or thread
  is started by `Engine` construction/ticks. Native TCP creation and accepted streams set NODELAY.
  Native socket send/receive buffers are configured to 256 KiB. SIGPIPE is suppressed on POSIX.
  TCP exposes the accepted source endpoint for session validation; EOF in a partial frame is diagnosed.
- `net/SocketTransports.h`: explicitly registered UDP endpoints (1200-byte datagrams), bounded
  TCP length framing (uint32 little-endian, at most 64 KiB), partial reads/writes and lifecycle
  events. `TcpFrameReader` accepts split/coalesced frames and rejects oversized lengths before
  allocating. WebSockets use native binary message boundaries with the same channel interface.
- TCP/WebSocket limits: 64 peers, 256 queued frames/4 MiB per peer; 16 I/O operations per peer
  per frame; 128 received messages per peer per frame. Connect timeout is 300 explicit frames.
  Native accept processes at most eight connections per frame; UDP drains at most 256 datagrams
  total and 128 per registered peer. Same-frame polls do no work. Unknown/oversized UDP packets
  are dropped without peer allocation or replies. Malformed/over-rate streams are disconnected.
- Browser JS validates binary size (64 KiB) before copying into Wasm. Callbacks buffer at most
  128 messages/4 MiB, drained only by transport Poll. Browser send buffering is capped at 4 MiB.
  Destruction removes event handlers before releasing the C++ callback owner.
- `net/FallbackTransport.h`: initially send over established TCP while known UDP addresses
  exchange bounded echo probes. A successful challenge enables UDP; periodic fresh challenges
  detect a later outage. Probe failure switches permanently to TCP, with incoming TCP data still
  accepted during/after route changes. At most one probe response per peer per frame. Nonzero
  connection-specific challenge seeds are supplied by M4; probes are not authentication.
- Disconnected transport events release channel buffers and produce one `Disconnected` event;
  connected transport events do not automatically register/authenticate channel peers.
- [x] Windows Release clean build and incremental build; 106 `oe_tests`, zero failed checks.
  Coverage: `NetworkTcpFramingAndBounds`, `NetworkUdpTcpFallbackReachability`,
  `NetworkNativeUdpSourcesAndTruncation`, `NetworkNativeTcpMalformedAndClose`,
  `NetworkChannelsOverLocalhostSockets`, `NetworkNativeFallbackAndChannelDisconnect`,
  `NetworkNativeTcpQueueAndLargeFrames`. Tests deliver 64 KiB messages over real localhost
  UDP/TCP, exercise split/coalesced stream frames, queue bytes/count limits, unknown/oversized
  UDP datagrams, malformed TCP lengths, resource cleanup, UDP outage/blackhole fallback and
  channel disconnect cleanup. Existing seeded loopback tests remain green. The inactive pin
  now also checks that native/browser socket creation and live-object counters stay unchanged.
- [x] `node tests/network_web_test.js`: shipped JS bridge tested with mock WebSockets for
  binary-size rejection before Wasm allocation, send buffering, empty messages, callback cleanup,
  closed/unsupported sockets and handle exhaustion. This is not a real browser/SDK test.
  CLI `oe exec samples/Hello sim.step '{"frames":120}'`: `ok: true`, frame 120, zero script errors.
- [x] Repair localized MSVC/Ninja header dependency detection: CMake probes raw `/showIncludes`
  bytes without code-page conversion. Verified with a fresh build-directory configure and recorded
  dependencies for `LoopbackTransport.cpp`; the old cache had zero dependencies and stale objects.
  No new compiler warnings; clean build exposed pre-existing editor C4458 warnings in
  `EditorTiles.cpp` and `Editor.cpp`, outside this milestone.
- [ ] POSIX native build/test: no installed WSL/Linux toolchain in this environment. Run
  `cmake -S . -B build -G Ninja -DOE_GPU=OFF`, build, and `build/bin/oe_tests` on Linux/macOS.
- [ ] Real browser/Wasm compilation and integration: `build_web.bat` reports Emscripten not
  found. Build with the SDK, run Node-compatible tests and connect a browser client to a binary
  WebSocket test server. The native framed TCP listener is not a WebSocket server.
- [ ] Android compiler/device validation and runtime rebuild: `build_android.bat` reports NDK
  not found. `runtime/web/` and `runtime/android/` were not rebuilt. When network project/session
  integration is enabled, add Android manifest `INTERNET` permission for enabled projects (implemented in M4).
- M4 must supply session/cookie validation and connection-incarnation isolation before registering
  game peers. Reachability probes do not authenticate endpoints. IPv6, asynchronous native DNS and
  public-network congestion control are follow-ups. No commands/components changed in M3, so
  `docs/API.md` and user-facing READMEs remain unchanged.

### M4 implementation notes

- `net/Session.h/.cpp` owns the lobby and a transport-independent cookie gate before channels.
  `app/EngineNetwork.cpp` implements the shared command/Lua surface. No session is created by
  project loading or by a project with absent/`none` networking; enabled projects open sockets
  only on explicit `net.host`/`net.join`. Networking advances once at the beginning of
  `SimulateFrame`; events are sorted by player id then RPC sequence and dispatched after script
  updates, before built-in systems. A scene change through `game.loadScene` preserves the lobby;
  `sim.stop`, edit-time scene loads and project changes destroy it.
- Configuration accepts `lockstep`/`authoritative` for M4 lobbies only (`syncImplemented:false`).
  `rollback` is rejected until state snapshots exist. Tick rate is exactly 60; max players 1..64;
  port 0..65535; numeric IPv4 bind defaults to loopback. `gameId` defaults to the project name,
  with a 1..64 printable-byte limit. Different game ids, modes, tick rates or session protocol
  versions fail the join with an explicit mismatch error. Future input/snapshot settings are
  reserved for M5/M6. No entity replication, frame gating or player input merge exists yet.
- Native host listens on TCP; `transport:udp` additionally binds UDP on the same numeric port.
  TCP exchanges Hello, Challenge, Response, Welcome and confirmation (session protocol v1).
  The host assigns monotonic player ids (host=1, no reuse within an incarnation), readiness and
  a uint32 seed (`net.host {seed}`, default 1). Lua RNG uses that seed and is reseeded when a
  joining client receives Welcome. Scripts may run while connecting; M4 does not guarantee
  world convergence or synchronized RPC execution frames.
- Cookies are two uint64 values from platform cryptographic entropy (Win32 BCrypt, POSIX
  `/dev/urandom`, browser `crypto.getRandomValues`). No simulation uses OS randomness.
  Only bounded TCP accepted connections or explicit in-process loopback endpoints can handshake;
  channels and UDP registration happen only after the cookie response. UDP endpoints use the
  TCP peer IP plus negotiated UDP port. TCP always carries handshake/control traffic; UDP control
  packets are rejected. UDP reachability failure keeps authenticated channel traffic on TCP.
- Every channel datagram is inside a 24-byte magic/cookie/blob envelope, validated before packet
  decoding/reassembly. `kNetPacketBytes` is now 1176 (1124-byte fragment payload) so the complete
  UDP datagram stays within 1200 bytes. Fresh cookies reject old packets after reconnect, even
  when a transport id is reused. Channel ACK/replay windows and monotonic RPC sequences reject
  duplicates within a connection. Cookie possession is **not identity authentication**; v1 is
  unencrypted and is not protected against an on-path attacker. No cookies are reported/logged.
- Bounds: 64 accepted/pending peers, project max-player admission, eight incoming control messages
  per peer/frame, 1024-byte controls, 300-frame handshake/connect timeout, 600-frame inactivity
  timeout and 60-frame keepalive. Reliable channel limits still apply. Graceful leave/kick drains
  acknowledgements for at most 30 frames; terminal states release listeners, sockets, peers and
  queues. Slow roster queues close their peer rather than stall other players.
- Commands: `net.state`, `net.host`, `net.join`, `net.leave`, `net.players`, `net.ready`, `net.kick`,
  `net.stats`, `net.rpc`. They do not participate in scene undo. Tools must keep a session alive
  (`oe script`, MCP/editor or HTTP) and advance both engines with `sim.step`; a one-shot `oe exec`
  host exits immediately. `state`, `players`, `stats` are safe to read with networking disabled.
  `net.simulate`, local-peer spawning and dedicated/editor tools remain M7; desync reports M5.
- Lua provides the same operations plus role queries, `net.localPlayer()`, sorted id array
  `net.players()`, `net.on(name, fn)` and `net.sender()` within a handler. RPC targets are
  `server`, `all`, `others`, or a decimal player id; `owner` needs M6. Arguments: at most 16 finite
  JSON values, depth <=8, strings <=1024 bytes, collections <=64 items and <=8192 serialized
  bytes; Lua conversion also bounds traversal at 512 nodes. At most 64 handlers; `net.on(name,nil)`
  removes one. Local recursive RPC is limited to eight callbacks and shares its instruction budget.
  Session observer queues hold at most 256 events; overflow is dropped/counted without blocking.
  Games must revalidate RPCs; routing does not grant clients authority over scene state.
- None mode: server=true, client/host=false, local player=1, Lua players={1}; RPC calls the local
  handler immediately (except `others`, which has no recipient). `owner`/missing players are errors.
  Host/join are refused with a project-setting hint; no sockets, sessions or network polls occur.
  Shared API integer validation rejects nonfinite/out-of-range values before integer conversion;
  oversized or nonfinite seed arguments cannot trigger undefined casts.
  Loopback rooms are scoped to one process, created only by hosts, and removed on host shutdown.
  Late joins map their private frame counter to the room wire clock; reconnect does not wait to
  catch up with an existing host. `net.spawn_local_peers` and fault controls remain M7.
- Android APK/AAB manifests request `android.permission.INTERNET` only for enabled network
  projects. Default/none manifests retain their previous permissions.
- [x] Windows Release build; 112 tests, zero failed checks. New coverage:
  `NetworkNoneLuaRpcAndLimits`, `NetworkEngineLoopbackLobbyAndRpc`,
  `NetworkSessionConfigValidation`, `NetworkSessionHandshakeLossVersionAndTimeout`,
  `NetworkSessionCookieReplayBoundsAndReconnect`, `NetworkEngineNativeSessionTcpUdp`.
  Covers real TCP/UDP host/join/RPC, 3-player roster/readiness/kick/leave/reconnect, callbacks,
  reproducible loss/duplication/reordering, version rejection, timeout, malformed/replayed cookies,
  RPC bounds and inactive session/socket/poll counters. Android manifest checks cover opt-in
  INTERNET permission in binary XML and protobuf. Existing determinism/editor tests remain green.
- [x] Node JS bridge mocks, including entropy availability/failure, size guards and callback cleanup.
  API reference regenerated and persistent CLI script smoke checks passed (see validation commands below).
- [ ] POSIX build, real browser/Wasm connection and Android device verification: no WSL/Linux
  toolchain, Emscripten SDK or Android NDK installed. Run the M3 SDK commands when available;
  rebuild `runtime/web/` and `runtime/android/` before distributing these new APIs. Committed
  runtimes remain unchanged. Web clients require an external binary WebSocket endpoint that
  speaks the session protocol; the native TCP listener does not implement WebSocket upgrades.

M4 validation commands:
```text
build.bat
build/bin/oe_tests.exe
node tests/network_web_test.js
oe api --markdown > docs/API.md
```


### M5a/M5b — lockstep and reference rollback (2026-10-02)

Lockstep is implemented; full-state replay and correction are implemented as an experimental
reference backend. M5 remains unchecked because fast native snapshots have not been implemented.
The reference backend preserves correctness for opaque Lua closures/timers and physics solver
history without pretending that scene JSON alone is a snapshot. It also restores allocation gaps,
save memory and audio voice ids, mixer position, event history and captured PCM.

Configuration (in `project.json`):
```json
"network": {
  "mode": "lockstep",
  "transport": "udp",
  "port": 7778,
  "actions": ["W", "A", "S", "D", "Space"],
  "axes": ["LeftX", "LeftY"],
  "inputDelay": 2,
  "hashInterval": 60,
  "waitFrames": 300,
  "dropPolicy": "kick",
  "rollbackFrames": 8
}
```

- `actions` declares literal key names, at most 64 unique printable ASCII names of 1..32 bytes;
  `axes` declares up to six standard gamepad axes. Held and pressed bitfields are independent.
  Raw axes are quantized to signed 16-bit values; Lua applies the existing dead zone after decoding.
  Missing actions/axes default to empty arrays. Pointer coordinates/deltas, touch ids and viewport
  dimensions are not encoded: use declared actions/axes for this backend. Plain `input.*` sees the
  synchronized local input; each `input.player(id)` sees the same sorted merged frame on every peer.
- After host/join and readiness on every player, the host calls `net.start`. Begin/ready/go verify
  the input schema, seed and initial scene/resource fingerprint, freeze the roster, and reset all
  peers to the scene captured when play began, at frame zero. The first `inputDelay` frames are
  neutral. Lobby callbacks/RPC run before the barrier; match scripts initialize after the barrier.
- `sim.step` advances I/O attempts, so `Engine::Frame()` may advance fewer times while waiting.
  Real-time ticking also continues transport polling while the game is gated. `net.state.sync`
  contains frame, confirmed count, waiting, predicted count, correction count and terminal error.
  `net.stats.replayMilliseconds` measures the most recent complete reference restoration.
- `inputDelay` is 0..8, `rollbackFrames` 1..8, `hashInterval` 1..600, and `waitFrames` 30..600.
  `dropPolicy: "kick"` kicks missing remote inputs and stops the frozen match after the I/O timeout;
  `empty` substitutes neutral input and continues. A disconnect stops the match. Stop/leave and
  host/join establish another lobby. Arbitrary RPC, readiness changes, late joins and external
  gameplay edits/evaluation are disabled during matches to keep arrival time out of simulation.
  Session controls also refuse synchronized Lua game callbacks, so speculative/replayed frames
  cannot emit leave/kick side effects. Use game frame/time for gameplay; RTT, confirmation and
  correction diagnostics are observer data, not deterministic simulation inputs.
- Authenticated match messages use the existing bounded reliable ordered channel over any
  transport. This deliberately retains head-of-line blocking. There is no input packet exposure
  before the M4 cookie gate. Match messages cap at 60000 bytes; sync queues cap at 256 messages/
  4 MiB, future input at 32 frames, and retained coordinator history at 32 frames. Protocol version
  is now 2; v1 clients are rejected rather than silently disagreeing about synchronization.
- Confirmed periodic Fnv1a64 diagnostics cover reflected scene state plus game data and audio
  voices/events, excluding device/capture preferences and local transport counters. The first
  mismatch stops all peers, emits `onNetState("desync")` and `net.on("net.desync", handler)`, and
  exposes `net.desync_report`: frame, player, decimal hash strings, hostScene and peerScene.
  Diagnostic scene JSON includes a `runtime` object with game/audio data. Each scene text caps at
  24000 bytes; larger scenes omit the texts and set `scenesOmitted`, but hashes are still checked.
  Opaque Lua upvalues and native solver internals are restored by replay, not directly hashed.
- Set `mode: "rollback"` to predict missing remote held/axis input (pressed is never repeated),
  at most `rollbackFrames` ahead of confirmation. A differing authoritative frame replaces the
  journal from the earliest mismatch; one shared world routine reconstructs all execution from
  frame zero. Only confirmed audio blocks reach speakers/capture; already submitted audio is
  preserved through corrections. Hot reload is disabled while a match/recording is active.
  Reference matches stop at 12000 game frames; lockstep matches have no such lifetime cap.
- Packaged resource fingerprints cover sorted project files except project.json, AGENTS.md,
  CLAUDE.md, tools and dotfiles, at most 4096 files. Keep these files immutable. Automatic replay
  refuses changed resources; manual state loading also checks before altering the world.
  All simulation-affecting settings/resources and initial save values must match peers.

Standalone full-state recording is explicit, so ordinary single-player execution allocates no
journal and no FrameSync. Call `sim.record_state` before simulation or Lua evaluation, then
`sim.save_state {slot}` / `sim.load_state {slot}`. Slots are in-memory (eight slots, names 1..32
bytes), not portable save files. Each journal caps at 12000 frames and 512 external Lua evaluations
of at most 64 KiB. Evaluation calls after recording are themselves replayed; arbitrary C++ scene
mutation or direct `ScriptHost::Eval` outside `Engine::Call` is outside the recording contract.
Runtime components/closures created before recording are also outside that contract. Editor undo
is independent. Directory save slots are frozen at recording; browser localStorage games must
preload needed slots before recording. Save flushes stay in memory until recording/match stops.
Speaker output is suppressed during restoration and captured output is restored exactly.

Validation:
- [x] Windows Release build; 120 tests, zero failed checks, no new compiler warnings.
- [x] 2- and 4-player authenticated sessions each merge 10000 frames with seeded loss,
      duplication and reordering; summed deterministic worlds agree.
- [x] Two real localhost engine peers over TCP and UDP each reach 10000 game frames with
      confirmed hashes agreeing; no mock socket substitution.
- [x] Frame waiting/recovery, forced prediction correction, complete small-scene mismatch reports,
      large-world hash checks, schema mismatch, malformed/future input, kick and empty timeout policies.
- [x] Snapshot tests replay Lua closures/timers/RNG, Jolt and Box2D bodies, dynamic entity creation/
      deletion, allocation gaps, scene changes, audio mixer state and exact PCM; changed-resource loading is refused.
- [x] Inactive mode still pins identical frame hashes, no FrameSync/session/channel/socket creation
      or networking polls. Existing rendering, editor, gameplay and save tests pass.
- [x] API regenerated; persistent CLI barrier and snapshot smoke tests and JS bridge mocks.
- Reference benchmark (Windows Release, default template game, one restoration per sample):
  2000 recorded frames restored in 110.56 ms; 5000 in 267.64 ms. These are fixture measurements,
  not worst-case bounds, and exceed the 16.67 ms frame budget. M5 is therefore not marked complete.
- [ ] M5c: replace O(history) reference restoration with complete native snapshots (Lua closures,
      timers, RNG, Jolt/Box2D solver/contact history, scene runtime pools and audio). Add a measured
      worst-case restoration budget for representative games and remove the reference history cap.
- [ ] POSIX, real browser/Wasm and Android device execution and refreshed prebuilt runtimes:
      Emscripten, NDK and Linux toolchains remain unavailable. Rebuild and run on each platform
      before shipping M5 networking there; existing committed runtimes do not contain M5 APIs.
