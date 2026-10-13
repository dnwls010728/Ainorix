# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-13 with Emscripten 6.0.10, Release, from main (48b1742). A2 preview now defaults
to excluding only its target's stored BoneIK goals (ik:false); ik:true opts in. State exposes ik/ikGoals,
and attachments follow the resulting preview pose. EngineAnimation.cpp and Pose.cpp are shared player
code, so both web and Android players were rebuilt. The correction passed all 373 Windows tests,
including goal scope, free/fixed foot motion and unchanged authored state/frame hashes. Chisa Walk
0.20/0.60 s editor screenshots confirmed different leg poses; its project files remained untouched.
Includes A2 isolated
animation.preview/preview_state/preview_close and preview screenshots. The native Animation panel is
a desktop authoring tool, not shipped player UI. Windows verification: 371 tests, with the final editor
case also covering legacy layout migration/persistence; isolated preview software/D3D11 mean difference
0.083/255. The refreshed player ran the model/sprite demo in headless Edge WebGL2 without script errors.
Includes A1 model/sprite
timeline events, model event sidecars, animation.events/set_events and onAnimationEvent callbacks
before physics, with loop/reverse/completion handling and native snapshot replay support.
See [ANIMATION.md](../../docs/ANIMATION.md). All earlier runtime changes remain included, including R9:
PostProcess screen-space reflections (ssr, ssrDistance, ssrThickness), fixed 64-step rays with five
hit refinements, material-aware reflected-ambient replacement and probe/sky fallback. All earlier
runtime features remain included; see [POSTPROCESS.md](../../docs/POSTPROCESS.md).
Packaging still patches the game's key into the runtime for keyed OEPAK002 archives and stripped Lua bytecode.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,633 | 01cab76470319e28cb0ab53673bf1628a9256d96a4bd2704f3153de0186b9a88 |
| oe_player.wasm | 6,195,115 | 863d81d3a7843e7c25c53ad8b858ad7522744fc92f3c862a88359b82270fb88b |

Windows Release: 368 tests pass, including four A1 event tests. The A1 800x500 software/D3D11 demo's
mean RGB difference is 0.089/255. Headless Edge (WebGL2 on SwiftShader) ran both model and sprite
callbacks and HUD updates without script/WebGL errors; the packaged model sidecar loaded correctly.
Earlier R9 verification:
The 800x500 SSR demonstration's software/D3D11 mean RGB difference is 0.146/255, and SSR-off Showcase
retains hash 72ab07fb213fd42b. Headless Edge (WebGL2 on SwiftShader) compiled/linked the mesh and SSR
programs, displayed glossy reflections and switched SSR/SSAO/DOF/bloom targets without WebGL errors.
SSR uses four color attachments and a separate five-texture pass; the mesh budget remains 208 vec4
vectors plus 16 sampler uniforms (224 conservatively), with 16 texture units. Float color-buffer
support is required. Minimum-device skin/vertex-graph execution remains unverified. Earlier checks
are preserved in Git history.

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
