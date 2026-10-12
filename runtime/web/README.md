# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-13 with Emscripten 6.0.10, Release, from main (d24d743). R2 adds the SpotLight
component, soft cone attenuation and an 8x9 RGBA32F light-data texture shared with sky/probe irradiance.
See [LIGHTING.md](../../docs/LIGHTING.md). All earlier player features remain included; earlier build
history is preserved in Git. Packaging still patches the game's key into the runtime for keyed OEPAK002
archives and stripped Lua bytecode.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,633 | 3ad966f135787b1393f8806f79de30cfcadcfb414ddfc06bc7b4126edd91e8e6 |
| oe_player.wasm | 5,929,528 | 36e713fc3dcef9caa7d1e8ffe40f5698caec71f8abc65384e76b82cf4378f001 |

Windows Release: 346 tests pass. Headless Edge (WebGL2 on SwiftShader) rendered the packaged spotlight
demo and the existing reflection-probe room without shader/WebGL warnings. The room's probe captured
128-pixel faces in 185 ms. The shader has 12 texture units and 187 fragment uniform vectors, plus twelve
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
