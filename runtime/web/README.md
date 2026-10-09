# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-09 with Emscripten 6.0.10, Release, from branch `claude/editor-startup` (shaders without the
loops the D3D compiler had to unroll: the shadow lookup moved out of the directional-light loop, `GraphFinite`
without a loop). It contains every engine feature up to then, including the
GPU performance work of docs/RENDER_PERF.md (shadow fit without the CPU vertex stage, depth of field from the scene
pass's second target, frustum culling, dynamic batching, `shadowDistance`, shader graphs compiled to GLSL ES): scripting and official
Lua modules, physics (Jolt, Box2D), UI widgets, saves, animation, gamepads and multi-touch, host bridge,
networking (WebSocket client, protocol v4), asset-based VFX with expressions, surface shader graphs (vertex offset,
normal, `Sprite.material`), post effects (tone mapping incl. ACES, bloom, DOF, FXAA, fog, colour grading),
hemisphere ambient, `Sprite.emission`, glTF `COLOR_0` and the `LeftBracket`/`RightBracket` key names.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,633 | cc2d3d2aed2277b9e88da446b7678132920ca424209736304046658d338e27ba |
| oe_player.wasm | 5,038,696 | db3aa262b713d7978bae0cad790f07fb9beb6b1b595b07fe466d507f63a9d0e6 |

Windows Release: all engine tests pass. `games/RPG` packaged with it runs in headless Edge (WebGL2 on SwiftShader):
compiled shader graphs (swaying trees, terrain, water, sky), shadows, the depth-of-field MRT target and fog render
with no shader compile warnings. Not verified in a browser for this build: frame rate on real GPUs, typing into `UIInput`,
real ads, sound output and phone touch.
A browser can join binary WebSocket sessions but cannot host or use raw UDP/TCP (`link.open` returns `nil`).
Earlier per-build notes are in the git history of this file.

## Rebuild

Rebuild after changing engine C++ used by the player. Commit both files with the source change and update the
table above. Scene, Lua and asset edits need packaging, not a runtime build.

```bat
set EMSDK=C:\path\to\emsdk
set BINARYEN_CORES=1
build_web.bat
```

The build writes `build/bin/web` and `runtime/web`. Packaging prefers `build/bin/web` over this committed fallback
and reports the selected `webRuntime`. Binaryen's parallel optimizer crashes on some Windows hosts, hence
`BINARYEN_CORES=1`. To repeat the Wasm test suite, configure `build-web` with `-DOE_BUILD_TESTS=ON`, build
`oe_tests` and run `node build-web/bin/oe_tests.js`.
