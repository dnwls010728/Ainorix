# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-13 with Emscripten 6.0.10, Release, from main (b745881). R4 adds opt-in
PointLight.shadows and SpotLight.shadows, deterministic two-light selection, perspective bias and
512-square maps in a 2048-square local-shadow atlas. Light data is now 8x18 RGBA32F. R2 spotlights,
R3 cascades and all earlier player features remain included; see [LIGHTING.md](../../docs/LIGHTING.md).
Packaging still patches the game's key into the runtime for keyed OEPAK002 archives and stripped Lua bytecode.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,633 | 01cab76470319e28cb0ab53673bf1628a9256d96a4bd2704f3153de0186b9a88 |
| oe_player.wasm | 5,985,995 | c78bab58a35aaf045641d0800c3a03c76a3182f15f4afb96c1ca87fb5533a010 |

Windows Release: 351 tests pass, including all six point faces, two points using twelve tiles, spot
occlusion, caster toggles and software/GPU comparisons. Headless Edge (WebGL2 on SwiftShader) compiled
and linked the generated shaders and rendered the packaged point/spot example without shader/WebGL
warnings. The fragment program has 203 vec4 uniform vectors and 13 sampler uniforms (216 conservatively),
using 13 texture units, within GLES3/WebGL2 fragment minimums. Earlier R3/R2 browser checks are in Git history.
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
