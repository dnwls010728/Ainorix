# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with index.html and game.pak; packaging needs no Emscripten.

- Built: 2026-10-04, Emscripten 6.0.10, Release (`build_web.bat`, BINARYEN_CORES=1).
- Source: the Dongle Zoo branch plus the UI widget work (`UIButton` textures/icons/toggles, keyboard/gamepad
  focus, `UIInput` with typed text from `KeyboardEvent.key`, caret/selection and an in-game clipboard,
  `UIDropdown`, `UIJoystick`, images on every UI element): host bridge (`host.call` / `host.poll` / `host.language`, the page's
  `window.OEHost` and `Module.oeHostSend`), `input.viewSize`, on top of the VFX toolkit (particle
  curves, `Trail`), the Water/HD-2D work and everything listed before (protocol v4 sessions, lockstep,
  rollback, authoritative replication, saves, animation, gamepads, multi-touch, postprocessing,
  surface shader graphs).
- 2026-10-05 rebuild: `network.admission` + `net.admit`, `game.arg` (a page has no launch parameters: always `nil`)
  and the `link` table (browsers have no TCP: `link.open` returns `nil` and a message). Windows 198 tests; not run in a
  browser.
- 2026-10-04 rebuilds: local input in authoritative matches (pointer, typed text and undeclared keys reach the local
  UI on clients), then `network.lateJoin`, peers that keep simulating after a stopped match, `game.quit` (the page stops its
  frame loop and keeps the last picture) and `net.name` (`games/OnlineRPG`). Windows 196 tests; not run in a browser.
- Validation of the build before that: Windows 194 tests. The Wasm test suite was not rebuilt for this runtime, and the UI widget work
  (typing into a `UIInput` in a browser in particular) was not run in a browser. Before that work, the packaged
  `games/DongleZoo` was driven in headless Edge (WebGL2 through SwiftShader) over the DevTools
  protocol: page load, `host.js` loaded (`window.OEHost`, `Module.oeHostSend` present), Play clicked,
  animals dropped and merged, save written to localStorage, a host message queued and drained by the
  game. Real ads (AdSense account), sound output and touch on a phone browser were not checked.
- Browser can join binary WebSocket sessions, but cannot host or use raw UDP/TCP. Android
  runtimes are separate and remain pending. See [NETWORK_SAMPLES.md](../../docs/NETWORK_SAMPLES.md).
- Binaryen's parallel optimizer crashes on this Windows host; use BINARYEN_CORES=1.

## Rebuild

Rebuild after changing engine C++ used by the player. Commit both files with the source
change and update this README. Source/Lua/art-only game edits need packaging, not a runtime build.

```bat
set EMSDK=C:\path\to\emsdk
set BINARYEN_CORES=1
build_web.bat
```

The build writes build/bin/web and runtime/web. Packaging prefers build/bin/web over this
committed fallback and reports the selected webRuntime. Enable OE_BUILD_TESTS in build-web,
build oe_tests and run `node build-web/bin/oe_tests.js` to repeat the Wasm suite.

## Lua M1a-3 rebuild (2026-10-06)

Rebuilt with Emscripten 6.0.10 from E:/emsdk, Release, BINARYEN_CORES=1
and CMAKE_BUILD_PARALLEL_LEVEL=2. This supersedes earlier pending-build notes.
The embedded identity/module loader and ContentHash prefix must ship in the player,
so packaging new game data alone cannot provide this change.

Source provenance: base commit `eca56058cf63b79e0535d5f16c6ea33fcb3a3148`
plus the uncommitted working tree on 2026-10-06, including Lua M1a-1/2/3 and
the pre-existing networking changes. This is not a build of the base commit alone;
no commit was created. Official primitive revision: 1; manifest fingerprint:
`9ba77240e176a039`. Source SHA256: entities.lua
`0899331c3ca85b5687e7ccb5ed2351019bd67da20ebbbee8ef09001364f586f0c`,
ui.lua `fb8745dbb35f36cc6dadddf65a5b95eb448c54c444d838df026107aa1d0c49e9`.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| oe_player.wasm | 4,295,326 | d578178b03a6e617a8de9c3a6ccd09356cb17b95885380e93750c0b8649b3eb2 |

Verified: build_web.bat exit 0; oe_tests Wasm target rebuilt and run under
Emsdk Node 24.19.0 (186 tests, zero failed checks, exit 0); isolated OfficialSmoke
project packaged with oe package --web and served with oe serve. Its game.pak
contains only project.json, scene and probe script, with no official Lua files.
The Codex in-app Chromium browser displayed "Embedded modules work", logged
M1A3_IMPORT_OK and exposed data-official-smoke=ok plus epoch 0000000000000002
after asserting the official alias/cache/focus/handle imports. Files were served
from a temporary directory outside the engine checkout. Server and tab were cleaned up.

Headless Edge launch was rejected by automatic execution approval (no detailed
reason); the browser connector also reported Edge unavailable. The in-app browser
was the verified alternative. This does not claim an Edge run or audio/network/UI
interaction coverage. Existing Clang aggregate-initializer warnings in network
sources and the existing unused ProcessFixtureDir Wasm test helper remain;
no new warning in changed M1a-3 engine code was observed.

## M1b widget pilot rebuild (2026-10-06)

EMSDK=E:/emsdk, Emscripten 6.0.10, Release, BINARYEN_CORES=1, parallelism 2.
Base commit eca56058cf63b79e0535d5f16c6ea33fcb3a3148 plus the uncommitted
working tree, including prior M1a/networking changes and M1b widgets; no new commit.
Primitive revision 1, official digest 58adf79c7598d716.
oe/ui.lua SHA256 20c1f0b93dccb91e22f3497bcb5734d713da0672da6504ee662ed34a70b65684.
The UI source is embedded: packaging Lua game data alone cannot update it.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/web/oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| runtime/web/oe_player.wasm | 4,297,870 | f9047fecf6ccdbd70ca8d1e5322cdf95df7e989b03c71709562cd81fcb077670 |

The final player rebuild ran once after official sources were finalized, exit 0.
Desktop 223 tests and actual-process OnlineRPG end-to-end pass; five desktop
pilot images have identical before/after hashes/PNG bytes. This runtime contains
capability-aware widgets and the new official source agreement fingerprint.
Web JS was copied but its bytes are identical to M1a-3. The Wasm bytes changed.
M1b-specific Wasm/Node and browser execution were not run; the prior M1a-3
results above are historical and do not verify the new widget surface in a browser.

## M2a coordinate query rebuild (2026-10-06)

EMSDK=E:/emsdk, Emscripten 6.0.10, Release, BINARYEN_CORES=1, parallelism 2.
Base checkout commit da530f75508eac9f2ec63266aba94aa9761925f2 plus current uncommitted source, including
pre-existing networking/M1a/M1b/editor work. No commit was created.
The final web player rebuild ran once after M2a engine/official sources were finalized,
exit 0. Camera queries use shared renderer matrices and the existing canvas scale.
New native primitives are additive: existing primitive contracts and revision 1 remain
unchanged; the new oe.ui.screenToCanvas forwarding changes official digest
58adf79c7598d716 -> 2b06cf1f108cb7cf. Matching players must be deployed together.
Source SHA256: CoordinateQueries.cpp 49c5854a7e1aa38e1f72ba772747909282027e3f70306e79cb5fdd9ca0555f29;
oe/ui.lua 4abc66f722a27f1aedd900a0add0bf604cafe397b62a7a76cf11a6aa6bf34293.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/web/oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| runtime/web/oe_player.wasm | 4,319,405 | c66a6be5c6bf7cc17f6dced9f4c1b9403ab7e81dc4ae1466041989342e290998 |

JS was rebuilt/copied with identical bytes; Wasm changed to include the native camera/canvas
queries and refreshed embedded source/digest. Browser/package interaction was not repeated
in M2a; prior browser evidence above is historical.
M2a Wasm verification: configure OE_BUILD_TESTS=ON, build oe_tests, then Emsdk Node
24.19.0 build-web/bin/oe_tests.js: 190 tests, zero failed checks, exit 0. Both coordinate
and M1b widget/identity tests pass. The pre-existing unused ProcessFixtureDir warning remains.
Desktop 226 tests and actual-process OnlineRPG end-to-end pass; all eight software pilot
screenshots preserve hashes and PNG bytes. Source/projection tests and measurement details:
docs/LUA_USABILITY.md 6.4/8. Existing network aggregate-initializer warnings remain.

## M2b text measurement rebuild (2026-10-06)

EMSDK=E:/emsdk, Emscripten 6.0.10, Release, BINARYEN_CORES=1, parallelism 2.
Base checkout da530f75508eac9f2ec63266aba94aa9761925f2 plus the existing uncommitted working tree and
M2b changes. No commit was created. Final player rebuild ran once after engine/official
source finalization, exit 0. Additive native ui.measureText shares actual renderer shaping;
existing primitive behavior/revision 1 remain unchanged. Official digest
2b06cf1f108cb7cf -> dff0818781f4b2b4 (oe.ui forwarding); matching players required.
Source SHA256: UI.cpp 85b1c3be6eab9a4cb074a4ba51a6be853355af7f11a177ce029af42174909bf1;
CoordinateQueries.cpp 54358fc2e031c8d592280f7509ccbc9ce43bc0194b813485154cdcfa7186d33b;
oe/ui.lua 864206a35a6cb1cce51c88f014815e3c1917c0907b4b88ffb30ca27d97c043b0.
Embedding normalizes UTF-8 line endings; source hashes above identify working-tree bytes.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/web/oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| runtime/web/oe_player.wasm | 4,326,485 | f3bd188e478b78dd59f9b350e13e1f4751b33941d47d95b71480f39925185d25 |

JS rebuilt/copied with identical bytes; Wasm changes include text query code and new
official source/digest. After test build completed, Emsdk Node 24.19.0
build-web/bin/oe_tests.js: 192 tests, zero failed checks, exit 0.
Isolated TextSmoke project packaged --web outside the checkout and served on port 17862.
Codex in-app Chromium/WebGL2 rendered Embedded modules and text queries work;
M2B_IMPORT_QUERY_OK/M2B_BROWSER_OK and DOM status confirmed oe.ui/oe.entities aliases,
handles, measureText (2 lines, width 94.234375, height 56.25, scale 1), and all four M2a
coordinate queries. game.pak includes project.json, scene and probe script only; no
official source checkout. Tab and owned server PID cleaned. This is a smoke test, not
full browser pointer/audio/network acceptance. Logs/artifacts: build/m2b/.
Desktop 228 tests and OnlineRPG actual-process end-to-end pass. Six new multilingual
images change intentionally with measured pill widths/bubble heights; unchanged UI controls
preserve prior hashes. Existing network initializer and Wasm unused-helper warnings remain;
a new test range-copy warning was fixed before final suite rebuild. Shared fixture folders
require desktop/Wasm suites to run sequentially; interrupted/concurrent attempts are recorded
in docs/LUA_USABILITY.md 6.5/8, not counted as successful validation.

## M3 input/math runtime rebuild (2026-10-06)

Base checkout da530f75508eac9f2ec63266aba94aa9761925f2 plus the existing uncommitted
working tree and M3 changes; no commit. New embedded oe.input/oe.math sources and
manifest are shared by every player; C++ primitives and protocol v4 are unchanged.
Official manifest digest dff0818781f4b2b4 -> 403839f0fff280c5, primitive revision 1.
Digest computed using OfficialManifestDigest canonicalization and core/Image.cpp:240
seed; UTF-8 LF normalization. Source SHA256 (working-tree bytes):
input.lua 6b98002a2beb53661de61d60ea30ce24ffe65ebe607210696b65dbadeaaedf1b;
math.lua 65823eb38d15810a1c99f812c6d3f276b287eac876dbbea93bf7ba979ed71d50.

Final builds and tests exited 0. Desktop 230 tests; sequential Node/Wasm 194 tests.
Initial runtime rebuilds were superseded by the final reviewed-source rebuilds:
review found input.player's lobby fallback to device input, so playerSource now
requires a running match; math rejects false-valued coordinates too. Two runtime
build passes occurred, contrary to the requested single final pass; final bytes
below contain these corrections. Existing Wasm unused ProcessFixtureDir warning
remains; no new module warning was observed. Details/logs: build/m3/.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/web/oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| runtime/web/oe_player.wasm | 4,331,275 | 1931846ac88cc315b36790fbe0650d642d0697acf0656895664573c1c2f83de3 |

EMSDK=E:/emsdk, Emscripten 6.0.10, Release, BINARYEN_CORES=1, parallelism 2.
JS copied with identical bytes; Wasm changed to include new official Lua sources/digest.
Final web package outside checkout (InputSmoke, game.pak 1378 bytes) served on 17863.
Codex in-app Chromium/WebGL2 verified oe.input/math imports and aliases, zero normalize,
oe.ui measurement, then real D key press moved Transform.x from 0 to 1 through a
named button pressed action. Screenshot: build/m3/browser-keyboard.jpg; DOM m3.moved
and M3_INPUT_MATH_IMPORT_OK / M3_KEYBOARD_MOVED observed. This verifies edge/button
movement, not physical held-stick/controller input. Touch/UIJoystick on an actual
browser/device remains unverified (headless injected touch test passes). Server PID
and agent tab cleaned. Browser smoke repeated after the final rebuild.

## Required-only Lua module reload runtime refresh (2026-10-07)

Source: HEAD da530f75508eac9f2ec63266aba94aa9761925f2 plus the preserved dirty working
tree and LUA_USABILITY.md 6.10. No commit. Players contain dependency-aware transactional
project reload and the ordinary Lua snapshot allocator. The final pass adds same-table
forwarding safety and prevents project aliases from mutating cached official exports.
Initial gameplay, protocol v4 and official manifest digest 403839f0fff280c5 / primitive
revision 1 remain unchanged; all official Lua sources unchanged.

Native final build/full 235 tests and Node/Wasm full 199 tests exit 0. Native network,
snapshot, GPU/software and both suites' reload regressions pass. Two player build passes
were required because review found a shared-export alias bug after the first pass;
this deviates from the planned single final runtime pass. These hashes are corrected bytes.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/web/oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| runtime/web/oe_player.wasm | 4,357,125 | 5b75e1444fe8ed8272338359cab0c270ffcf91ed1fad4db6c5b9b97742dc96c5 |

Engine reload source SHA256 (working-tree bytes):

engine/script/ScriptHost.h: `ee08ab68c6d68f428d97fab48ba95b14a26cb7fe03961c7ef9e30fe23264d9e7`
engine/script/ScriptHost.cpp: `03729acf2e30d0bcb7120297487a074e04f9e9e031675e4edb1736161e9eb8eb`
engine/app/Engine.h: `41a400e0f5e1a70821ee6f025374774f61445ddfdf9c8dd44f93bdd20c7a21b2`
engine/app/Engine.cpp: `5bdb778f086a514eaf94ad86fa4b2852435a0ec82041d43aab3eaf7a3c109d0a`
engine/api/Commands.cpp: `b325687e72073839315c80ec5c117362d659ae884cb4ca108197f1be4ef4adc0`

build_web.bat, EMSDK=E:/emsdk (Emscripten 6.0.10), Release, BINARYEN_CORES=1,
parallelism 2. Wasm grew by 25,850 bytes for loader/transaction/alias safety; JS recopied
but byte-identical to M3. After final player build, configure tests ON and build/run only
oe_tests with Node. One existing unused ProcessFixtureDir warning. No new packaged browser
smoke in this unit; Node does not verify physical/browser input.
Final evidence: ignored build/reload/*release* and *reviewed-build* logs, artifacts.json.

## Orthographic Scene-view support runtime refresh (2026-10-07)

Source: main HEAD a822d27 plus the working-tree editor window-placement,
independent 2D/3D view persistence and orthographic free-camera changes.
The player-shared command parser now accepts `camera.projection: "orthographic"`
and `orthoSize` for matching screenshots and picking; default perspective and
scene-camera rendering are unchanged. Official Lua sources/digest are unchanged.
Commands.cpp SHA256: `eacd7f7bab83df0c74963fa6ef8fcf1d571b4a02bb63ca27fca91de362231df4`.
Native build and full suite: 245 tests, 0 failed checks, exit 0. The editor 2D
screenshot was visually inspected. No new packaged browser or Android device
execution was performed; Wasm tests were not rerun in this unit.
| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/web/oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| runtime/web/oe_player.wasm | 4,358,582 | 1f457fef76dafcf793ae7e720a82e7265235bcffd58e026874908115d539dd42 |

build_web.bat, EMSDK=E:/emsdk, Release. Two initial link attempts failed in
wasm-metadce (exit 3221225477); retry with BINARYEN_CORES=1 and build parallelism
2 succeeded. Wasm grew by 1,457 bytes; JS is byte-identical. Evidence:
ignored build/orthographic-web-build.log.

## VFX phase 4 rebuild (2026-10-08)

The player includes VFXSystem assets, materials, normalized age/random/speed graph inputs, optional
emitter batching, bounded death chains, two-anchor ribbons and static mesh particles. Unbatched
particles keep shared GPU meshes and use a dedicated draw input; batching uses vertex RGBA/data.
Legacy particle hashes remain unchanged. See docs/VFX_PHASE4.md for validation and known limits.

Built with Emscripten 6.0.10, Release, BINARYEN_CORES=1 and parallelism 2. Node/WASM portable
core: 230 tests, zero failures. Actual browser WebGL2 execution of these effects remains unverified.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/web/oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| runtime/web/oe_player.wasm | 4,677,178 | ebac03f51f5d5b6a3f7af13a1ac2e38b42c049c1b728c8ab640f6846659b67d8 |

Commands.cpp SHA256: `9e32bd478583e35831c86902197af9d2458133654dae8ba999c392db624fed74`.

## VFX motion channels rebuild (phase 5a, 2026-10-08)

Players include captured birth state, motion/XYZ scale curves, pivots and four custom material
attributes in both rendering paths and batching. See docs/VFX_PHASE5A.md for the contract and validation.
Built with Emscripten 6.0.10, Release, parallelism 2 and BINARYEN_CORES=1.
All 234 Node/WASM tests pass, including the same curve-proof hash as Windows. The existing
unused ProcessFixtureDir warning remains. Device/browser GPU execution remains unverified.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/web/oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| runtime/web/oe_player.wasm | 4,734,722 | e94db0c29e7239a1c40ed7c1516efd3cbd6a4f56328afc97cc3b082db220ba0f |

## VFX expressions rebuild (phase 5b, 2026-10-08)

Players include typed Birth/Update expressions, own bounded math, cached programs, parameter inputs
and conservative instruction budgets. Contract: docs/VFX_PHASE5B.md. Windows/Node assert the same
math and software-preview hashes; all 239 Node tests pass. Device/browser GPU execution remains
unverified. Toolchains: Emscripten 6.0.10 / Android NDK r28c, Release, parallelism 2.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/web/oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| runtime/web/oe_player.wasm | 4,803,292 | d2a340496cae0d3cfc6304a74f59f5919e36e50dace0b082f6e897988fbb0a82 |

Commands.cpp SHA256: `e9777ebc426db0bf619d384f6c28522ba625809c22eab950e52bc9aa6e62fa29`.

## VFX value-node authoring rebuild (phase 5c, 2026-10-08)

The compiler retains exact decimal literal spelling for transient graph conversion. Common commands
now expose graph/source conversion with the existing runtime evaluator. Simulation and legacy hashes
remain unchanged. All 244 Node/WASM tests pass. Desktop value-node editing is not part of the player.
Built with Emscripten 6.0.10 / Android NDK r28c, Release, parallelism 2 and BINARYEN_CORES=1 for Web.
Android devices, browser GPU and Linux/EGL execution remain unverified. See docs/VFX_PHASE5C.md.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/web/oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| runtime/web/oe_player.wasm | 4,830,223 | bdbcf6a5e5ecff4a81b237fa1f5c05cb2e0d24316f5d55782901cfd1e67be133 |

Commands.cpp SHA256: `bc3347cc460c58d067ce65920814b7f6984926421bae3169fc73ff740cd8f90d`.

## VFX acceptance preset release (phase 5d, 2026-10-08)

The six expression acceptance effects and their meshes/textures/materials now ship in the shared
engine. `vfx.create {path,preset}`, shared Effects drafts and linked preset placement preserve
parameters and budgets; dependency conflicts refuse overwrites. Fresh-project previews retain all six
software reference hashes. All 247 Node/WASM tests pass. Windows/editor evidence is recorded in
[the release report](../../docs/VFX_PHASE5D.md). Rebuilt with Emscripten 6.0.10 / NDK r28c, Release.
Android devices, browser GPU and Linux/EGL execution remain unverified.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/web/oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| runtime/web/oe_player.wasm | 4,921,115 | eee728a55e564efb4083986dfb1dd1d7f758a3cddfbb8908e341e1186c0081ba |

Commands.cpp SHA256: `c93cd01e47925ba2f8ea60087c51c98a37ea137ec3fc61e966235192e3222d07`.

## VFX phase 6 rebuild (2026-10-08)

Players author particles only through linked `VFXSystem` assets: the standalone ParticleEmitter scene
component, `particles.*` commands, the Lua `particles` table and `self:burst` are retired, and
`vfx.clear {restart}` / `vfx.state {particles:true}` follow docs/VFX_PHASE6.md. Built from main
d5a0141 with Emscripten 6.0.10, Release, BINARYEN_CORES=1, parallelism 2. Windows 305 tests and
Node/Wasm 249 tests pass with zero failed checks. JS is byte-identical to phase 5d. Android devices,
browser GPU and Linux/EGL execution remain unverified.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/web/oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| runtime/web/oe_player.wasm | 4,937,034 | cba63fd25fc346cc988007e1fd0816a80d01ca9f70c849d11a87dd06b949517d |

Commands.cpp SHA256: `524d9d4927ff3f93bc59578fc897c4d747f0791d5e90402b904d1f928361a00f`.

## Fog, grading and RPG rebuild (2026-10-08)

Players gain per-surface distance fog (`PostProcess.fog*`, material `fog: false`), ACES tone mapping,
colour grading (`saturation`, `contrast`, `shadowTint`, `highlightTint`), hemisphere ambient
(`DirectionalLight.hemisphere/groundAmbient`), `Sprite.emission`, `Sprite.material`, glTF `COLOR_0` and the `LeftBracket` /
`RightBracket` key names, used by games/RPG (docs/RPG.md). Built from branch claude/rpg-village on main 7cc9d8b
with Emscripten 6.0.10, Release, BINARYEN_CORES=1, parallelism 2. Windows 307 tests pass. JS is
byte-identical to the previous build. Browser GPU, Android devices and Linux/EGL execution remain unverified.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| runtime/web/oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| runtime/web/oe_player.wasm | 4,952,310 | 1d43af71f0b825e5ebaab64792640e54f25144f392863c102d946b33ec19e496 |
