# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-13 with Emscripten 6.0.10, Release, from the A3/A4/A5 working tree based on
main (ba403ec; A3/A4/A5 are not committed). Includes shared sprite .anim.json sheet/rectangle-atlas definitions,
model .clips.json metadata (displayName, loopRange, events), animation.create/get/set, dependency hot
reload and shared event authoring. A4 adds variable frame durations with cumulative marker times,
last-frame hold, shared playback/preview/render sampling and animation.get_clip/set_clip sequence
authoring; frame operations preserve event anchors. Decimal loop seams use float playback precision without rewriting
authored marker times. All earlier A1/A2 features remain: timeline callbacks/replay, isolated preview
with IK off by default and attachment following. The native Animation panel is a desktop authoring tool.
Animation assets, playback and preview belong to the shared OwnEngine library, so all players were
rebuilt. R9 SSR and earlier runtime features remain included. See [ANIMATION.md](../../docs/ANIMATION.md)
and [POSTPROCESS.md](../../docs/POSTPROCESS.md). A5 adds validated .controller.json asset commands,
embedded oe.animator Lua evaluation, typed parameters/ordered transitions and model blend seconds.
Shared C++ and the embedded module required rebuilt players. Packaging still patches the game's key for keyed
OEPAK002 archives and stripped Lua bytecode.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,633 | 01cab76470319e28cb0ab53673bf1628a9256d96a4bd2704f3153de0186b9a88 |
| oe_player.wasm | 6,273,203 | f85d1af663fbff2c1680c7448c96326b581181432d6f672062a69da4bd56c54a |

A5 verification: four controller tests cover file isolation/validation, trigger priority/failure behavior,
model crossfades, sprite/model native rollback and real native graph input. The packaged A5 model/sheet/atlas
demo ran in headless Edge WebGL2 (SwiftShader), loading controller files from game.pak and entering
Walk, Run, Jump and Idle without script/WebGL errors. Software/D3D11 mean RGB difference: 0.077/255.

Windows verification: all 385 tests pass, including four A4 timing/authoring/native UI tests and four A3 asset/API tests for sharing, isolation,
validation, atlas sampling, source hot reload, decimal loop seams and native replay. The 1000x600
model/sheet/atlas demo's software/D3D11 mean RGB difference is 0.066/255; Showcase retains hash
72ab07fb213fd42b. The packaged A4 variable-duration demo ran in headless Edge (WebGL2 on SwiftShader), loading shared
files from game.pak and emitting both foot callbacks for model, sheet and atlas without script/WebGL
errors. The final native snapshot benchmark passed at 6.25 ms against its 16.67 ms limit. Earlier skin/vertex-graph execution on minimum-capability devices remains unverified.

Not verified on real browser GPUs or phones: frame rate, UIInput typing, sound output, touch, ads, and
newer animation/physics/networking paths beyond the earlier recorded browser checks. A browser can join
binary WebSocket sessions but cannot host or use raw UDP/TCP (link.open returns nil).

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
