# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-13 with Emscripten 6.0.10, Release, from main (c059b98 + three fixes: a kinematic body
moved by a script keeps its sweep when particles or a raycast query the world before the step, a planar
reflection shows the Skybox background, spot lights wider than 60 degrees cast shadows through six faces).
The build before it (d428219) added R9:
PostProcess screen-space reflections (ssr, ssrDistance, ssrThickness), fixed 64-step rays with five
hit refinements, material-aware reflected-ambient replacement and probe/sky fallback. All earlier
runtime features remain included; see [POSTPROCESS.md](../../docs/POSTPROCESS.md).
Packaging still patches the game's key into the runtime for keyed OEPAK002 archives and stripped Lua bytecode.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,633 | 01cab76470319e28cb0ab53673bf1628a9256d96a4bd2704f3153de0186b9a88 |
| oe_player.wasm | 6,114,796 | b5ce15f4d6bce88cd24ac4c9622377225ec7b5f4573102e11796d0e90acc5233 |

Windows Release: 361 tests pass; the final cleanup also passed 14 camera tests and GPU comparison.
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
