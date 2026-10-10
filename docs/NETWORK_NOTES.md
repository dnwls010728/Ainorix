# Networking — implementation notes and work log

The design rules and the surface for games are in [NETWORK.md](NETWORK.md); read that first. This file is the work
log (docs/DESIGN.md §5) and the notes written while each milestone was implemented: wire formats, bounds, recording
contracts, limits and what was verified on which platform. Section numbers such as "§7" refer to NETWORK.md.

Entries are history. They mention sample projects, games, scripts and docs that have since been removed from the
repository (`samples/*`, `games/OnlineRPG`, `games/DongleZoo`, `NETWORK_SAMPLES.md`, `ONLINERPG_SERVERS.md`,
`tests/network_browser_fixture.py`, `tests/online_rpg_test.py`); the engine behaviour they describe is still covered by
the `Network*`, `Link*` and snapshot tests in `tests/tests.cpp`.

## Implementation status (work log)

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
- [x] M5 — lockstep and full-state rollback
  - [x] M5a — input merge, frame gating, ready barrier, desync reports, `input.player(id)`
  - [x] M5b — exact reference `Engine::SaveState/LoadState` and prediction/correction by replay
  - [x] M5c — fast native Lua/Jolt/Box2D snapshots; restore cost bounded by rollback window,
        benchmarked against the 1/60 s game budget (reference replay is not this backend)
- [x] M6 — authoritative: `NetSync`, snapshots/deltas, interpolation, prediction, relevance
- [x] M7 — headless dedicated server (`oe serve-game`, `--server`), editor Players×N play +
      Network panel, `net.simulate`
- [ ] M8 — networked sample games, docs (`API.md`, `SCRIPTING.md`, `PLATFORMS.md`), web and Android
      verification, optional relay/lobby
  - [x] M8a — playable lockstep co-op, rollback versus and authoritative samples; lobby/touch controls, regression tests and launch docs
  - [x] M8b — rebuild the protocol-v3 web runtime, run Wasm tests and exercise a packaged browser client against a native WebSocket server
  - [ ] M8c — rebuild Android runtimes, run device tests and a native-server/device match
  - [ ] Optional relay/lobby service — deferred; direct hosting and the in-game ready lobby require no external service

Open questions / unverified:
- M5c native snapshots and exact Lua/Jolt/Box2D restore are tested on Windows and Wasm/Node;
  full cross-device/Android determinism still requires device runs.
- Native WebSocket hosting and a real packaged Wasm browser client are verified in M8b;
  Android/POSIX builds and Android player execution remain unverified in this environment.
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
  detect a later outage. Probe failure switches permanently to TCP. The route only selects the
  **send** path: registered peers are received on both TCP and UDP at all times, because the two
  sides choose independently and can disagree after a short outage (see the audit follow-up). At most one probe response per peer per frame. Nonzero
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
- Bounds: 64 accepted/pending connections (unauthenticated ones do not occupy player slots;
  `maxPlayers` is enforced when the cookie response is accepted), eight incoming control messages
  per peer/frame, 1024-byte controls, 300-frame handshake/connect timeout, 600-frame inactivity
  timeout and 60-frame keepalive. Reliable channel limits still apply. Graceful leave/kick drains
  acknowledgements for at most 30 frames; terminal states release listeners, sockets, peers and
  queues. Slow roster queues close their peer rather than stall other players.
- Commands: `net.state`, `net.host`, `net.join`, `net.leave`, `net.players`, `net.ready`, `net.kick`,
  `net.stats`, `net.rpc`. They do not participate in scene undo. Tools must keep a session alive
  (`oe script`, MCP/editor or HTTP) and advance both engines with `sim.step`; a one-shot `oe exec`
  host exits immediately. `state`, `players`, `stats` are safe to read with networking disabled.
  `net.simulate`, local-peer spawning and dedicated/editor tools are implemented in M7; desync reports in M5.
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
  catch up with an existing host. `net.spawn_local_peers` and fault controls are implemented in M7.
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
  `empty` substitutes neutral input and continues: after one `waitFrames` timeout the player is
  marked stalled (`net.state.sync.stalled`) and its missing frames are neutral without further
  waiting, until one of its inputs arrives within 32 frames of the confirmed frame again. A client
  that fell behind steps up to four extra confirmed frames per tick and may buffer 3600 confirmed
  frames; beyond that it stops with "fell too far behind the match". A disconnect stops the match. Stop/leave and
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
  not worst-case bounds, and exceeded the 16.67 ms frame budget at M5b. M5c below supersedes that backend.
- [x] M5c: replaced O(history) reference restoration with complete native snapshots (Lua closures,
      timers, RNG, Jolt/Box2D solver/contact history, scene runtime pools and audio). Add a measured
      worst-case restoration budget for representative games and remove the reference history cap.
- [ ] POSIX, real browser/Wasm and Android device execution and refreshed prebuilt runtimes:
      Emscripten, NDK and Linux toolchains remain unavailable. Rebuild and run on each platform
      before shipping M5 networking there; existing committed runtimes do not contain M5 APIs.

### M5c — native bounded restoration (2026-10-02)

This supersedes the M5b reference replay backend and its 12000-frame cap. `sim.record_state`
remains explicit and must precede frame zero and any Lua execution. Snapshots are in-process,
owned by their originating Engine; they are not a portable save format. Project resources stay
immutable, hot reload is disabled during recording, and restoring changed resources is refused.

Lua uses an address-stable allocator in every VM (required-module transactional reload,
LUA_USABILITY.md 6.10). Ordinary frames still take no images or journal; recording remains
explicit and must precede Lua execution. An actual offline reload captures a temporary
image for failure recovery and is forbidden during recording/active matches. An image retains the live
blocks and their bytes, preserving closures, timers, coroutines, GC state and RNG without running
Lua during restore. Discarded speculative branches do not invoke finalizers. Scene images clone
all component pools, including hidden runtime values, and entity allocation cursors. Jolt saves
its full state plus body creation settings/ids and CharacterVirtual state; deterministic native
body ids allow creation/deletion across branches. Box2D's unmodified 3.1.1 adapter captures the
world struct and its tracked allocation graph, including broadphase, contacts and warm starts.
Its global allocator callbacks are scoped and serialized; each snapshot retains its world slot.
Audio mixer/voice state, pending unconfirmed PCM, UI state, scene changes, game data and save
slots are captured. Confirmed PCM is preserved across automatic corrections without duplicate
speaker/capture output. These images are version-specific and never accepted from network data.

- [x] Native rollback checkpoint ring contains at most rollbackFrames + 1 images; input journal
      retains only that window plus boundary inputs. Match history has no restoration cap.
- [x] Existing closure/timer/RNG, scene change, dynamic 2D/3D entities, exact future-state/PCM,
      input correction and zero-network-cost tests pass.
- [x] Windows Release: 121 tests, zero failures. Four representative samples (Hello, Dungeon,
      Platformer, FPS), 2000-frame history, 20 restores: measured maximum **5.301 ms** including
      resource validation, below 16.67 ms. This fixture budget is asserted in native tests, not
      an arbitrary-world worst-case guarantee. 13000-frame history restores without replay.
- [ ] Browser/Wasm, Android and POSIX native execution remain unverified; rebuild committed
      player runtimes using their SDKs before shipping these APIs on those platforms.

### M6 — authoritative replication and owned prediction (2026-10-02)

The session protocol was **v3** at this milestone (now v4, see the audit follow-up); older native/prebuilt players fail the version handshake.
`net.start` uses a frozen ready roster and validates the initial scene, project content, input
schema and seed before a frame-zero barrier. The barrier times out after `waitFrames` (default
300 I/O ticks), including lobbies that have already waited longer than that before starting.
The server advances without waiting for client input: each remote stream consumes at most one
contiguous authenticated client frame per server step, otherwise holding keys/axes without
repeating pulses. Clients never send entity state or choose another player's input identity.
Unexpected kinds, spoofed identities, duplicates, old/future frames and invalid axes/actions are
rejected before simulation. Authoritative player ids fit signed 32-bit component fields and are
separate from entity references, so prefab instantiation cannot remap `owner`/`player` ids.

`NetSync` fields:
- `owner`: controlling player id, or 0 for server ownership. It must belong to the ready roster.
- `fields`: up to 64 `Component.field` keys using reflection. Each value is an object with
  `onChange` (default true), `always`, `ownerOnly` and nonnegative numeric `quantize` (step size).
  Default fields are Transform.position and Transform.rotation. `always` overrides delta omission;
  `onChange:false` also sends unchanged values. Include velocity fields for physics prediction.
- `prefab`: optional project-relative prefab path. An existing Prefab.path is the fallback.
- `distance`: relevance radius from the receiver's NetPlayer; 0 means unlimited.
- `team`, `teamOnly`: require the receiver's NetPlayer.team to match. Owners bypass both filters.
- `predict`: default true; owned entities predict, others interpolate.

`NetPlayer {player,team}` identifies the observer/controller. `network.playerPrefab` optionally
creates one root per ready participant at match start; preplaced NetPlayers are reused. Dynamic
prefab instantiation and destruction during server simulation produce reliable spawn/despawn
records in snapshots. Server-assigned network ids are monotonic within the match. Clients retain
netId/local-id mappings and translate replicated entity references in two passes. Mark replicated
parents with NetSync too; if a parent is irrelevant its descendants are also omitted. Prefab-local
unreplicated children instantiate with their root. Initially replicated scene entities are hidden
until relevant and can be restored from the shared initial blueprint without a prefab path.

Snapshots use the last acknowledged per-client world as their delta baseline. Baselines retain
16 snapshots; an evicted baseline falls back to a full image. Since protocol v4, snapshots and
their ACKs use the **unreliable** channel: a lost or late snapshot is never retransmitted, the next
one is again a delta against the last acknowledged image (spawn and despawn are part of that delta,
so they need no separate reliable event), and loss cannot stall the stream or fill the reliable
queue. Over TCP/WebSocket the stream itself still delivers in order. Begin/ready/go, client inputs,
stop and RPC stay reliable ordered. Payloads use the bounded binary encoding in `net/JsonCodec.h`
(interned keys, varints, float32 when exact; roughly a third of the JSON text for a typical world).
`snapshotRate` accepts 1..60 Hz (default 20), with fractional
rates distributed over fixed 60 Hz frames. At most 1024 entities, 64 selected fields/entity and
56000 encoded payload bytes are allowed; oversized worlds stop with an explicit diagnostic.
Field values are finite, depth <=8, at most 256 children/value and 8192 bytes/string. Queues,
retained baselines, unread updates and input histories are bounded. Policy errors or invalid
snapshot images stop the match before invalid state is simulated.

`ownerOnly` filters the transmitted fields; it does not hide constants already distributed in
scene/prefab/resource files or erase knowledge a previous owner had. Do not put secrets in shipped
project resources. Server RPC handlers must validate `net.sender()` according to the game's rules.

Clients retain local native checkpoints and input history for `predictionFrames` (8..64,
default 32). An acknowledged snapshot restores that checkpoint, applies server fields and replays
only remaining local inputs. Multiple snapshots in one poll reconcile the newest image while
retaining all interpolation samples. The input window pauses simulation rather than growing
without bounds. Local/unreplicated scripts and owned prediction run on clients; remote replicated
scripts are suppressed. Remote rigid bodies are kinematic and remote characters provide colliders.
Physics correction rebuilds solvers with corrected poses while preserving collision event history.
Author gameplay outside replicated entities with `net.isServer()` guards; local UI/camera scripts
can run normally, including relative mouse look (`input.mouseDelta`/`lockMouse`). A look direction
must travel in the input stream: `input.setLook` plus the `LookX`/`LookY` entries of `network.axes`
(RightX/RightY wire slots, no dead zone; see SCRIPTING.md). Remote float/Vec3/Color fields interpolate at `interpolationFrames` (0..30,
default 6, about 100 ms). Integer, boolean, string and entity-reference fields do not interpolate.

`net.entities` / Lua `net.entities()` expose network ids, local ids, owners and received fields.
`net.state` and `net.stats` include snapshot/input sequence, pending input count, correction count
and replication count. `net.rpc {target:"owner",entity:<id-or-name>,name,args}` resolves NetSync.owner;
Lua can use `net.rpc("owner",name,entityIdOrName,...)`, whose first argument remains in the payload.
RPCs remain available during authoritative matches. Prediction replay suppresses repeated sends
and journals incoming callbacks within a bounded 256-event window. Client audio defers speculative
PCM until acknowledged and preserves confirmed output across corrections. Save writes remain
deferred as in rollback. Hot reload and external scene/Lua edits are disabled during the match.

This milestone uses a frozen scene/roster (departures are handled, see M7; joining a running match came later as
`network.lateJoin`, see the audit follow-up): without it, late join and synchronized scene transitions require
leaving/stopping, opening the same scene and starting another ready barrier. M7 adds the dedicated server,
editor Players×N/Network panel and network fault commands; M8 samples/platform verification
remain unchecked. Neither absence of SDKs nor source portability is a device/browser execution test.

Validation:
- [x] Windows Release build; 127 tests, zero failed checks; no new compiler warnings.
- [x] Authenticated seeded Session/Authority runs for 10000 server frames with packet loss,
      duplication and reordering; the final reconstructed client world matches the server.
- [x] ACK-based deltas, unchanged/always fields, spawn/despawn, owner-only transmission, relevance,
      interpolation, bounded prediction/stall recovery, ownership spoof/future/malformed input,
      schema/version bounds and independent player/entity id namespaces are covered.
- [x] Real native TCP and UDP engine pairs exercise automatic player prefabs, replication and
      owned correction; a Box2D player predicts movement and reconciles to the stopped server pose.
- [x] Owner RPC routes both ways and does not repeat/vanish during prediction replay.
- [x] Two independent CLI processes over real TCP pass host/join/ready/start, input injection,
      automatic player spawning, net.entities and correction inspection; JS bridge mocks pass.
- [x] Native snapshot regression additionally covers Jolt body/character spawn/despawn with forced
      Lua garbage collection across multiple branches. Inactive networking still creates no
      Authority/FrameSync/session/channel/socket and keeps reference frame hashes unchanged.
- [x] API regenerated; scripting, code map, READMEs and runtime rebuild notes updated.
- [ ] POSIX, real browser/Wasm and Android builds/device execution: required toolchains absent.
      Committed Web/Android runtimes are unchanged and do not contain these M5c/M6 features.


### M7 implementation status (work log)

- [x] Dedicated headless runner: `net.serve`, `oe serve-game`, packaged player `--server`; ready barrier, bounded test run and shutdown.
- [x] Deterministic `net.simulate` fault controls on shared loopback rooms.
- [x] `net.spawn_local_peers`, peer inspection/input routing and deterministic group stepping/cleanup.
- [x] Editor Players selector, peer Game view/input, Network diagnostics and latency controls.
- [x] Windows Release build; 133 tests, zero failed checks, no new compiler warnings. Real TCP/UDP/WebSocket server/client CLI smoke, actual packaged --server, editor screenshot and API/reference updates pass.
- [ ] Web/Wasm player, Android and POSIX execution and refreshed prebuilt runtimes require unavailable SDKs/toolchains. Native WebSocket wire is verified with a standard Node WebSocket client.


#### M7 dedicated servers and local previews

`oe serve-game <project> [--port P --min-players N --seed S --frames N --api-port P]`
starts an explicit `net.serve` session. The packaged desktop player accepts
`Game.exe [project] --server [--port P --min-players N --seed S --frames N]` (default
project: `game/` next to the executable). Neither runner creates a window, GPU or
speaker device. The Windows GUI player attaches to an existing parent console without
creating one and preserves redirected streams. Ctrl+C/SIGTERM requests shutdown;
`--frames` bounds **I/O ticks**, including lobby waiting, rather than game frames.
The CLI prints startup and final JSON envelopes; the player prints its final envelope.
The optional HTTP API still binds only to 127.0.0.1 and accepts MCP attachments.

The ready barrier starts automatically once at least `minPlayers` remote peers have
joined **and every joined peer is ready**. The roster is frozen for joining: nobody can
enter a running match. A participant that leaves, times out or stops draining its reliable
queue is **dropped** from an authoritative match: its `NetPlayer` entities are destroyed, other
entities it owned become server-owned (`owner` 0), `onPlayerLeft(id)` runs and the remaining
players continue. The match (and the dedicated runner) stops when no remote participant
remains, or when anyone is lost during the ready barrier. Lockstep/rollback matches still stop
on any loss, because every peer simulates every player's input.
`sim.stop`/`net.leave` ends the runner; reopening/restarting creates a new lobby.
Dedicated servers reserve participant id **1** as an always-empty input stream; it
counts toward `maxPlayers`, so 4 slots permit 3 remote players (maximum 63 remotes).
`net.isServer()` is true, `net.isHost()` is false, and `net.localPlayer()` remains 1
for protocol compatibility. The server does not instantiate the playerPrefab for
slot 1; normal listen hosts still instantiate their own player. Server-authority
gameplay should use `net.isServer()`, not `net.isHost()`.

TCP and UDP listeners use the project transport and bind address; `--port 0` chooses
an available port. Set `network.transport` to `websocket` for a native RFC 6455
listener. Browser clients use `net.join {address:"ws://127.0.0.1:7778/game"}` with
matching mode, gameId, input schema and resources. The listener supports masked binary
messages, continuation frames, ping/pong and close; 8 KiB HTTP headers, 64 KiB assembled
messages, 64 endpoints, bounded transmit/receive queues and a 300-tick upgrade timeout.
Text/extension frames are rejected. TLS (`wss://`) requires a reverse proxy. No
browser can host sockets, and static hosting cannot replace this server process.

`net.spawn_local_peers {count:2,seed:71}` creates **two additional** preview engines
plus the invoking host, copies the current unsaved edit-time scene, switches only the
active sessions to loopback, readies the lobby and starts it automatically. Count is
1..7 and must fit the project's maxPlayers. Use it before a play session, after
`sim.stop`; projects with absent/none networking reject it. `sim.step` on the host
and ordinary editor Play ticks drive every peer once in stable index order. Preview
peers have independent Lua/physics/audio/save state and never open sockets, GPUs or
speaker devices. Stop, leave, scene load or project reopen destroys them and restores
the original transport. Save writes remain deferred during synchronized matches.

`net.local_peers` lists indices, frames, states, stats and desync reports; index 0
is the host. `net.peer_call` routes input/diagnostic commands and returns their full
envelope, for example:

```json
{"command":"net.peer_call","args":{"peer":1,"command":"input.key","args":{"key":"W","down":true}}}
```

Allowed calls: `input.*`, `sim.state`, `net.state/players/stats/entities/desync_report`,
`entity.get`, `scene.get`, `game.state`, `script.errors`, `render.screenshot`. Arbitrary
scene edits, peer stepping/stopping and recursive network controls are rejected.
These preview/fault/server controls are tools, not Lua gameplay bindings.

`net.simulate {seed:45,latencyFrames:2,jitterFrames:1,lossPermille:50,
duplicatePermille:100,reorderFrames:1}` configures the shared loopback wire. Delays
are fixed 60 Hz **one-way frames** (0..3600); loss/duplication are parts per thousand
(0..1000). Missing settings retain previous values. Each update resets the seeded
fault RNG for future sends; already queued packets retain their original deadlines.
No arguments reads settings. TCP/UDP/WebSocket fault mutation is rejected. RTT,
channel byte/loss counters and shared queued/dropped-wire counters appear in net.stats.

The editor adds Players 1..min(8,maxPlayers) beside Play for enabled projects. Players
1 preserves normal Play; larger values use the same spawn command. Game's Player
selector renders any peer through the existing GPU and routes keyboard/mouse/gamepad
commands to that peer. Switching peers or losing focus releases held input. View >
Network shows roster readiness, peer frames/state, RTT/loss/bytes/pending messages,
desync reports and a latency slider. `oe editor <project> --players 3 --play --frames
100 --screenshot build/network-editor.png` makes a reproducible preview screenshot.

Verification: Windows Release `oe_tests` covers fault-driven lockstep/rollback groups,
authoritative prefab previews, ready-gated dedicated TCP hosting and socket cleanup,
WebSocket handshake/framing/bounds/native listener, and editor peer input/Stop/PNG.
`python tests/network_server_test.py` exercises independent real TCP/UDP and standard Node
WebSocket clients, owned input/replication/correction, optional HTTP API, bounded CLI
shutdown and packaged `--server`. It needs the Release binaries and Node >=22.
`node tests/network_web_test.js` checks the existing browser callback bridge. Actual
Wasm/browser, Android and POSIX execution and rebuilt prebuilt players remain unchecked.


M7 editor visibility follow-up: Network is discoverable under View > Network for all
projects. A disabled project displays configuration guidance without starting a session
or changing project.json. Legacy panel masks no longer hide the new tab; old layouts
add it beside Inspector, new layouts use an explicit NetworkPanel visibility setting,
and reopening the panel focuses its tab. Windows regression tests cover disabled
projects, migrated docking, deliberate hidden-state persistence and zero socket/session
creation. Only editor code changes; prebuilt game players need no additional rebuild.


### M8a/M8b implementation notes

- `samples/NetCoop`, `NetDuel`, `NetArena` are playable crystal-collection games for lockstep,
  rollback and server authority. They share ordinary Lua player rules and built-in meshes,
  support offline practice, native lobbies, editor Players previews and keyboard/gamepad/touch.
  Co-op shares a score; versus awards individual scores and a five-crystal win. R resets a
  round through synchronized input. Authoritative score/target/HUD changes run on the server.
- Fixed on-screen action sampling: previously UpdateButtonKeys ran after sync input submission,
  so a network player reading input.player never received touch button actions. Local pointers
  now produce held/pulse actions before lockstep/rollback/authority sampling. Device edges stay
  outside replay checkpoints; replay does not resample local pointer positions. Short clicks
  count as a one-frame action. Ordinary offline input order and inactive costs are preserved.
- `NetworkSampleGamesOfflineAndMultiplayer` exercises all three games: offline win, two-peer
  previews, touch movement, short Collect click, remote touch Reset, matching HUDs, zero
  script errors/desync, and restored edit worlds on Stop. Native-only dedicated/listener tests
  skip under Emscripten; portable WebSocket codec/loopback tests still execute. Test output
  is unbuffered so an interrupted run retains the last completed test.
- Updated NETWORK_SAMPLES.md, API.md (regenerated; schema unchanged), SCRIPTING.md,
  PLATFORMS.md, ANDROID.md, agent instructions and all three READMEs. Direct hosting plus
  the in-game ready lobby is sufficient; optional relay/account/matchmaking remains deferred.
- Windows Release: 135 tests, zero failed checks. Wasm/Node: 123 registered tests, zero failed
  checks (native socket and GPU tests skip). `network_server_test.py` passes independent
  TCP/UDP/WebSocket client/server runs and a packaged `--server` run; JS callback bounds/cleanup
  pass. All three editor Players 2 screenshots were checked with D3D11. Emscripten 6.0.10
  Release player refreshed in runtime/web, with BINARYEN_CORES=1.
- Real packaged NetArena browser/WebGL2 client against a native WebSocket dedicated server:
  Join/Ready, keyboard movement, Collect button, server score {"2":1}, replicated HUD, Reset
  returning to empty scores and zero Lua errors verified. Fixture:
  `python tests/network_browser_fixture.py --sample NetArena --test-controls`; port flags
  select a free local triple. Other browser sync modes can use the same fixture but their
  browser/native cross-platform sessions have not been separately exercised in M8.
- [ ] M8c: no Android SDK/NDK or connected device in this environment. Rebuild both Android
  ABIs, run the staged tests and a TCP device/server touch match as described in
  NETWORK_SAMPLES.md; refresh runtime/android after actual build/device verification.
  M8 stays unchecked until this required validation is completed.

### Audit follow-up (2026-10-03)

A review of M1–M8 against this document found the defects below; each fix has a regression test.

- [x] UDP/TCP fallback: sides could select different routes after a ~0.5 s UDP outage (one kept
      UDP because its probes were still answered, the other had fallen back) and the UDP sender's
      packets were discarded until the reliable timeout closed the session. Both paths are now
      always received (`NetworkFallbackRoutesMayDisagree`).
- [x] Client interpolation advanced its playback frame twice per tick (before and after the
      simulation), so remote entities moved at double speed and stalled, with about half the
      configured buffer. It now advances once (`NetworkAuthoritativeInterpolationAdvancesOncePerFrame`).
- [x] One slow or departing client stopped an authoritative match for everybody and ended the
      dedicated runner. Departed players are dropped instead (M7 text above;
      `NetworkAuthoritativePlayerLossKeepsMatch`). `net.kick` no longer fails on an unresponsive
      peer: the connection is closed on the next poll.
- [x] Unauthenticated connections counted against `maxPlayers`, so idle sockets could keep a
      lobby closed (`NetworkSessionPendingConnectionsDoNotFillLobby`). They are now bounded
      separately (64). An attacker holding all 64 pending connections can still delay joins for
      the 300-frame handshake timeout; v1 has no authentication (§9).
- [x] `accept` failures other than a broken listener (reset before accept, descriptor/buffer
      exhaustion, a child that cannot be configured) are skipped and retried instead of failing
      the transport poll and closing the host session. Read from code; not reproduced with a real
      reset race, and the POSIX branch is not compiled here.
- [x] `save.flush` during a match/recording returned success without ever writing. Requested
      slots are now flushed when deferral ends (`SaveDeferredFlushCommitsOnStop`, SAVE.md).
- [x] Stale diagnostics: `net.state.sync.backend` reports `nativeRollback` (was `referenceReplay`),
      the unused 12000-frame constant is gone, and Session state reports `syncImplemented: true`
      for every enabled mode.
- [x] Validation: Windows Release 150 tests and Wasm/Node 137 tests, zero failed checks;
      `tests/network_server_test.py` (real TCP/UDP/WebSocket dedicated server, packaged
      `--server`) and `tests/network_web_test.js` pass. `runtime/web` rebuilt (Emscripten 6.0.10);
      the packaged browser match was not repeated for this rebuild. `docs/API.md` is unchanged.
- [x] `dropPolicy: "empty"` waited `waitFrames` for **every** frame a stalled peer missed (5 s per
      frame by default). The host now marks that player stalled after one timeout and substitutes
      neutral input without waiting; a late client catches up (up to four extra confirmed frames
      per tick, 3600 buffered) and is waited for again once its inputs are close
      (`NetworkFrameSyncStalledPeerCatchesUp`). Lockstep stays exact: only how many confirmed
      frames a peer simulates per tick changes.
- [x] Authoritative snapshots: unreliable delivery without head-of-line delay and a binary
      payload encoding (`NetworkAuthoritativeUnreliableSnapshotsConverge`,
      `NetworkJsonCodecRoundTripAndBounds`). **Session protocol v4**; v3 peers fail the handshake.
- [ ] Scale beyond 64 participants is still open: `maxPlayers`, the channel/transport peer tables,
      the roster message (8 KiB) and the 64-bit ready/roster encodings are all sized for 64, and
      the server builds one filtered world per client per snapshot. The "few hundred players" goal
      in §1 needs those raised and measured; the 1024-entity / 56000-byte snapshot caps remain.
- [x] `engine/api/HttpServer.cpp` no longer includes OS socket headers: the local API server and
      `oe mcp --connect` client run on `platform/Network.h` sockets (`HttpServerOnPlatformSockets`).
      A silent client is dropped after 15 s instead of blocking the server; port 0 reports the
      chosen port. Measured 17.6 ms per call in a burst through `oe mcp --port`, which is the
      tool main loop's sleep between job polls rather than the socket layer.
- [x] Validation for the four lines above: Windows Release 154 tests and Wasm/Node 141 tests, zero
      failed checks; `tests/network_server_test.py` and `tests/network_web_test.js` pass;
      `runtime/web` rebuilt for protocol v4. The packaged browser match was not repeated.
- [x] Local input in authoritative matches (2026-10-04, for `games/OnlineRPG`): a client's `input_` during a match was
      only the decoded stream sample, so UI clicks, `UIInput` typing and undeclared keys did not exist on clients, and
      text typed with `input.text` stayed in the device input forever. `Engine::MergeLocalInput` now adds the device's
      pointer, touches, wheel, text and undeclared keys after `ApplyInputs` (clients and listen hosts, not dedicated
      servers); `authorityLocal_` keeps that device input per unacknowledged input frame (pruned with the
      acknowledgement, cleared on start/reset) and replays merge it again, so focus and typed text survive corrections;
      `SampleNetworkInput` drops keyboard keys while a `UIInput` has the focus. Lockstep/rollback input is unchanged.
      Test `OnlineRPGGame`; `tests/online_rpg_test.py` with real processes. `runtime/web` rebuilt; Android not.
- [x] Late join for authoritative matches (2026-10-04, for `games/OnlineRPG`): `network.lateJoin: true`. The host does
      not seal its session at the start; every tick `Engine::AdmitAuthorityPlayers` invites lobby players that are
      ready and not yet participants (`Authority::Admit`): the newcomer gets the ordinary Begin (blueprint, schema,
      roster including itself), answers Ready, and the host adds it to the roster, sends Go to it alone and
      spawns its `playerPrefab` (`SpawnAuthorityPlayer`, shared with the match start). The client runs its usual
      start (scene from its play snapshot, replicated entities from the first full snapshot; its input frames start
      at 1, independent of the server frame). Invitations unanswered for `waitFrames` ticks are dropped; a player
      that leaves the lobby first is forgotten. Clients do not learn later roster changes, so with `lateJoin` they
      do not check snapshot owners against their roster. `net.serve` accepts `minPlayers` 0 (the default with
      `lateJoin`): the world starts at once, and `DropAuthorityPlayers` no longer stops a late-join match when the
      last remote player leaves. Limits unchanged: 64 participants, ids never reused (bounded by INT32_MAX).
      Test `NetworkAuthoritativeLateJoin` (server alone, join without ready stays in the lobby, two staggered
      players, both leave, rejoin); `tests/online_rpg_test.py` with real TCP processes.
- [x] A stopped authoritative match froze the peer: `PollAuthority` returned false for every state but idle, so after
      the server vanished no script or UI ran again and the game could not even say so. Only the ready barrier
      (`preparing`) holds frames back now; a stopped peer simulates locally. `game.quit` (Lua + command) lets a game
      close its window; `oe run` and the packaged player honour it, tools report `game.state.quitRequested`.
- [x] Loopback hosts created connections with `received = 0`, so once the host had run 600 frames every new
      loopback peer was dropped as silent before its handshake finished (`started`/`received` now both start at the
      current frame). Real sockets set it on accept and were not affected.
- [x] Validation for the three lines above: Windows Release 196 tests, zero failed checks; `runtime/web` rebuilt, not
      run in a browser; Android runtimes not rebuilt.
- [x] Generic support for games with their own back end (2026-10-05, ONLINERPG_SERVERS.md S1). (1) `LinkSet`
      (`engine/net/Link.h`): up to 8 outbound connections on one `TcpTransport`, so the wire is its framing with a
      `u16` type in front of the payload; pumped once per `SimulateFrame` (only when a link exists: no socket and
      no poll otherwise), closed in `ResetRuntime` because the ids live in the Lua state. Refused while a
      lockstep/rollback match is active and on a predicting client (a replay would open twice); host and stopped
      peers may use them. Lua `link.open/send/poll/close/encode/decode`, commands `link.*`. (2) Launch
      parameters: `Engine::SetLaunchArg`, CLI `--set name=value` / `--set-file name=path` on every `oe` command
      that opens a project and on the packaged player, Lua `game.arg`, command `game.args`. (3)
      `network.admission: "script"` + `net.admit`: `AdmitAuthorityPlayers` invites only players in `admitted_`
      (pruned to the lobby each tick, cleared with the authority); `net.serve` refuses `minPlayers` > 0 with it.
      `net.kick` is now allowed inside the world on an authoritative host (its state is never hash-compared).
      (4) `GameFiles` and `ContentHash` skip a project's `server/` folder. Tests `NetworkScriptAdmission`,
      `LinksAndLaunchArgs` (a `TcpTransport` listener as the service); Windows Release 198 tests, zero failed checks.
      Limits: IPv4 literals only, no TLS, no TCP in browsers, session protocol unchanged (v4).
- [x] Android runtime compilation refreshed for both ABIs on 2026-10-06 (Lua M1a-3), including protocol v4 networking; APK signed and externally validated.
- [x] A client could stop an authoritative world (2026-10-10, found with the separate Ainorix Online project): a
      client whose own ready barrier timed out (a tool stepping it faster than real time, a stall) sends `Stop`,
      and the host stopped its whole match for every player. `Authority::Receive` now treats a `Stop` from a remote
      participant of a running match as that player leaving (`TakeDropped`; the engine kicks it and removes its
      avatars, a non-late-join match still ends when its last remote participant is gone); only a `Stop` from the
      server ends a client's match. The kick reason of dropped players is "dropped from the match" (was "send queue
      overflow"). Test: `NetworkAuthoritativeDeltaAckAndBounds`; Windows Release 311 tests, zero failed checks.
- [x] Agreeing snapshots skip the correction (2026-10-10, measured with Ainorix Online): a predicting client
      restored its acknowledged checkpoint and replayed every pending frame on every snapshot, 20 times a second,
      even when its prediction was right; with a ~900-entity town a replay took 12-15 ms and a client frame averaged
      6.6 ms (peaks 24 ms). The client now records, per input frame, its own predicted entities' replicated fields
      the way the server sends them (`PredictedOwnedFields`, quantized); a snapshot whose owned records match within
      2e-3 (`PredictionAgrees`) only prunes what is confirmed. Remote entities, spawns and despawns were already
      applied every frame from the snapshots (`InterpolateAuthority`), so nothing else depends on the replay; a new
      owned entity, an Entity-typed field or any difference still takes the full correction. `net.stats` reports
      `agreedSnapshots` next to `corrections`. Measured: 3 corrections and 110 agreed snapshots in 4 s of walking,
      client frame 2.7 ms average, 3.8 ms peak. Test: `NetworkAuthoritativePrefabAndNativeTcpUdp`.
- [ ] Android device networking/import execution and POSIX native build/tests remain unverified; no device/emulator or Linux toolchain was used. Runtime compilation does not verify a session on Android.

### Official Lua agreement (M1a-3, 2026-10-06)

ContentHash unconditionally prefixes the project manifest with
`official-lua-v1:<primitiveRevision>:<digest>\n` (`engine/app/EngineSync.cpp:77`).
OfficialModules computes a sorted, LF-normalized, length-prefixed source fingerprint;
primitive-contract revision is currently 1. Paths, timestamps and local Scene epochs
are excluded. Bump the primitive revision when semantics used by the official helpers change.
FNV64 is a compatibility fingerprint, not authentication.

Lockstep/rollback (`EngineSync.cpp:103`) and authority (`EngineAuthority.cpp:72`) already
consume the same ContentHash. Authority's late-join barrier compares that same blueprint
(`engine/net/sync/Authority.cpp:89`, `:112`). Session protocol remains v4, with no new
wire fields or message types. Matching runtimes start; changed sources/revisions and an
old v4 project-only fingerprint are refused by the existing mismatch messages. A lobby
connection alone does not prove compatibility. Tests use a private friend provider in
Engine, with no CLI/Lua/launch-option override.

Recording must still be enabled before Lua execution/physics creation. Saved NativeState
restores the Lua heap, cached official exports, closures and private handles together with
Scene epoch. Loading against a different embedded fingerprint returns the existing
state_resource error before restoration (`EngineSync.cpp:225`). No hidden handle registry
or new snapshot field is needed. Desktop 221 and Wasm 186 tests pass, including all three
OfficialLua agreement tests and existing frame-sync/native snapshot regressions. Android
players were rebuilt and packaged, but device execution is unverified.
