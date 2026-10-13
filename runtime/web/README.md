# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-13 with Emscripten 6.0.10, Release, from main (975e58e). R5 adds opt-in
PostProcess.ssao/ssaoRadius/ssaoBias, a depth prepass and 32 fixed samples that darken ambient light
only. Direct light, emission, UI and transparent surfaces are unaffected. R2 spotlights, R3 cascades,
R4 local shadows and all earlier player features remain included; see [POSTPROCESS.md](../../docs/POSTPROCESS.md).
Packaging still patches the game's key into the runtime for keyed OEPAK002 archives and stripped Lua bytecode.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,633 | 01cab76470319e28cb0ab53673bf1628a9256d96a4bd2704f3153de0186b9a88 |
| oe_player.wasm | 6,005,988 | 08254f7647705f76eb1297c24ebd522e022dee7dbbf953a0f5b726706baa5cfc |

Windows Release: 352 tests pass, including SSAO contacts, perspective/orthographic views, exclusions,
determinism, effect combinations and software/GPU comparisons. Headless Edge (WebGL2 on SwiftShader)
compiled and linked the generated mesh program and rendered the packaged SSAO demonstration without
shader/WebGL warnings. The fragment program has 209 vec4 uniform vectors and 14 sampler uniforms
(223 conservatively), using 14 texture units, within GLES3/WebGL2 fragment minimums. SSAO requires
renderable R32F targets (float color-buffer support). Earlier browser checks are in Git history.
The pre-existing vertex skin/graph uniform budget is unchanged; minimum-capability vertex-stage execution
still needs device validation.

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
