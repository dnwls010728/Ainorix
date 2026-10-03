# Network samples and platform verification

Three small games use the same deterministic player/collection rules, with different sync models:

| Project | Model | Goal | State owner |
|---|---|---|---|
| `samples/NetCoop` | lockstep | Collect five gold crystals together | Every peer simulates all players |
| `samples/NetDuel` | rollback | First player to collect five wins | Every peer predicts and corrects all players |
| `samples/NetArena` | authoritative | First player to collect five wins | Server scores; clients predict their owned player |

All use built-in meshes, so no asset download or art generator is needed. WASD / left stick
moves a player; Space collects within 1.5 world units; R resets scores and the crystal route
without a scene transition. On-screen buttons provide the same actions for mouse/touch.
Players are blue, red, green and purple, in player-id order. The gold crystal follows a fixed
route, and ties are resolved by sorted entity id. No RPC accepts client score/position claims.

## Editor and offline practice

```bat
build\bin\oe.exe editor samples/NetCoop
build\bin\oe.exe editor samples/NetDuel
build\bin\oe.exe editor samples/NetArena
```

Choose **Players 2**, then Play. Switch the Game peer selector to control the other player.
The Network panel exposes seeded latency, jitter, loss, duplication and reordering. Up to
four players are configured; Players 1 runs offline practice. Stop restores the original scene.
An agent can use `net.spawn_local_peers {count:1, seed:71}`, `sim.step`, `net.peer_call` and
`net.simulate`; count excludes the host. Snapshot/frame diagnostics stay in `net.stats`.

## Two native players

Run `oe run samples/NetCoop` twice (or either other sample). Click **Host** on the first,
**Join** on the second, **Ready** on every peer, then **Start** on the host. Host is ready
immediately; repeated Ready is harmless. A premature Start reports the ready-barrier error.
Default transport is TCP, port 7778, bind/address 127.0.0.1. Only one server can own that port.

For a LAN session, edit `network.bind` to the server's numeric IPv4 address and the Join
button's `Script.params.address` to the same address **in a shared copy used by both peers**.
Change `network.port` and every lobby button's `params.port` together when using a different
port. Project settings take effect after reopening. Scene/scripts/prefabs must match exactly;
transport/address changes in project.json alone are excluded from the content fingerprint,
but changes to a lobby Script component in the scene are not. The samples use no lobby service.

## Dedicated authoritative server

```bat
build\bin\oe.exe serve-game samples/NetArena --port 7778 --min-players 1 --api-port 7798
build\bin\oe.exe package samples/NetArena --out build/NetArena-package
build\NetArena-package\NetArena.exe --server --port 7778 --min-players 1
```

Use one server command at a time, then run a client and click Join, Ready. The dedicated
server starts automatically when the required remote players are ready. Player id 1 is
reserved and gets no avatar; the first client controls red player 2. maxPlayers includes
the reserved slot, so this sample admits three remote players. Stop the process with Ctrl+C
or use the local HTTP API `net.leave`. Gameplay creates no window, GPU or audio device.
The packaged client still starts offline and uses the same in-game lobby.

## Browser client against a native WebSocket server

The refreshed prebuilt runtime includes protocol v4; packaging needs no Emscripten SDK.
A browser can join WebSocket sessions, but cannot listen for connections or use native TCP/UDP.
Native players currently cannot join WebSocket sessions either. Use a native `oe serve-game`
WebSocket host for browser players, and TCP/UDP hosts for native/Android players.

```bat
python tests/network_browser_fixture.py --sample NetArena --test-controls
```

This copies the sample into `build/network-browser`, sets WebSocket transport and a matching
Join URL **before** packaging, serves HTTP at 127.0.0.1:8098 and starts a native game server
at 127.0.0.1:7778 with its diagnostic API on 7798. Open the printed URL, click Join, then Ready.
Move to the crystal and click Collect; the server computes the score and replicates the HUD.
Reset clears the round. The optional DOM **Move to crystal** button sends a bounded 450 ms
A+W keyboard gesture for reproducible testing of the first dedicated client. It is present only
in the disposable test page. Ctrl+C stops both servers. Select a free port triple with
`--http-port`, `--game-port`, `--api-port`; select NetCoop/NetDuel with `--sample`.

For deployment, serve the packaged directory on a static host. Use a matching server-side
project and a shared lobby scene containing the public `wss://` URL. Put TLS at a reverse proxy
for an HTTPS page; the engine's WebSocket listener implements plain WS. Binding a LAN/public
address is explicit. Relay/matchmaking/account services are optional and deferred.

## Repeatable checks

```bat
build.bat
build\bin\oe_tests.exe
python tests/network_server_test.py
node tests/network_web_test.js
set EMSDK=C:\path\to\emsdk
set BINARYEN_CORES=1
build_web.bat
```

Configure the same `build-web` directory with `-DOE_BUILD_TESTS=ON`, build `oe_tests`, and run
`node build-web/bin/oe_tests.js` from the source root. CMake/Ninja must be on PATH (Visual
Studio bundles both). Binaryen on this Windows host requires BINARYEN_CORES=1 to avoid its
parallel optimizer crash. The full Windows suite passes 135 tests; Wasm passes 123, including
`NetworkSampleGamesOfflineAndMultiplayer`. Native socket/server tests skip in Wasm; portable
WebSocket framing and loopback sample gameplay still run. Node bridge mocks test callback
bounds/cleanup separately; they do not substitute for a real browser connection.

Sample regression drives an offline five-crystal win, two-peer ready/start, touch movement, a short Collect
click, remote touch Reset, replicated HUD agreement, zero Lua errors/desync and stop cleanup.
`network_server_test.py` uses independent native processes for TCP/UDP/WebSocket, plus a
packaged dedicated-server smoke check. Screenshots/rendering should be checked after UI changes.

## Android verification still required

The committed Android runtimes predate networking. Rebuild both ABIs with the NDK before
packaging these games. The current Windows environment has no Android SDK/NDK or connected
device, so no Android build/device result is claimed for M8.

1. Set ANDROID_NDK_HOME (and ANDROID_HOME for signing/device tools); run `build_android.bat`.
2. Enable OE_BUILD_TESTS, build/run oe_tests with the fixtures described in ANDROID.md.
3. Start the TCP NetArena server with an explicit LAN bind; configure the shared scene's Join
   address for that server. Package with `oe package <copy> --android --install --sdk <sdk>`.
4. Verify the signed APK with apksigner and its opt-in INTERNET permission. Join/Ready on
   the device; use touch movement/Collect/Reset. Check server input/score, snapshots, disconnect
   cleanup and `adb logcat -s OwnEngine` for zero script errors. Repeat on arm64 hardware and
   the x86_64 emulator, then refresh runtime/android and record the actual device results.

See [NETWORK.md](NETWORK.md) for milestone status and limits, [SCRIPTING.md](SCRIPTING.md)
for Lua contracts, and [PLATFORMS.md](PLATFORMS.md) / [ANDROID.md](ANDROID.md) for adapters.
