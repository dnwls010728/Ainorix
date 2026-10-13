# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-13 with Emscripten 6.0.10, Release, from main (35e3f58). R3 adds DirectionalLight.shadowCascades
(1..4), stable camera slices, blended transitions and a 2048-square depth atlas. The R2 SpotLight
component and light-data texture remain included.
See [LIGHTING.md](../../docs/LIGHTING.md). All earlier player features remain included; earlier build
history is preserved in Git. Packaging still patches the game's key into the runtime for keyed OEPAK002
archives and stripped Lua bytecode.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,633 | 01cab76470319e28cb0ab53673bf1628a9256d96a4bd2704f3153de0186b9a88 |
| oe_player.wasm | 5,950,528 | f2f4931e40fc4914161c78981b4c490c3b6529a1555a158a534b1d5b95f96292 |

Windows Release: 348 tests pass. Headless Edge (WebGL2 on SwiftShader) rendered the packaged four-cascade
demo without shader/WebGL warnings; atlas orientation and near/far shadows matched Windows. R2 previously
checked the spotlight demo and reflection-probe room (185 ms capture). The shader has 12 texture units
and 203 fragment uniform vectors, plus twelve
sampler uniforms, within the GLES3/WebGL2 fragment minimums. The pre-existing vertex skin/graph uniform
budget is unchanged; execution on devices exposing only the vertex-stage minimum remains unverified.

Not verified on real browser GPUs or phones: frame rate, UIInput typing, sound output, touch, ads, and
the newer animation/physics/networking paths beyond the earlier recorded browser checks. A browser can
join binary WebSocket sessions but cannot host or use raw UDP/TCP (link.open returns nil).

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
