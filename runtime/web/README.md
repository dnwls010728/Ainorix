# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-13 with Emscripten 6.0.10, Release, from main (258200c). R8 adds VFX emitter
collision (none/stop/bounce/die), birth-captured bounce/friction and 2D/3D choice, static-only batched
physics ray queries, world-pinned stops and contact death chains. Isolated previews do not have
colliders. No renderer/shader resources were added. All earlier runtime features remain included;
see [PARTICLE_COLLISION.md](../../docs/PARTICLE_COLLISION.md).
Packaging still patches the game's key into the runtime for keyed OEPAK002 archives and stripped Lua bytecode.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,633 | 01cab76470319e28cb0ab53673bf1628a9256d96a4bd2704f3153de0186b9a88 |
| oe_player.wasm | 6,077,553 | 2bfe8e8a4fb7119251c42c4c9eb4713f243bb6d862d7208687454005fb839120 |

Windows Release: 360 tests pass, including 3D/2D static collision, sensor/kinematic exclusion,
high-speed stop/bounce/die, nonuniform local motion, birth capture, native restore, motion channels,
and deferred onDeath children. The 800x500, 120-frame demo's software/D3D11 mean RGB difference is
0.429/255. Headless Edge (WebGL2 on SwiftShader) displayed the packaged 3D stop/bounce/die demonstration
without WebGL errors. Browser 2D particle collision was not separately checked. The mesh fragment
budget stays 208 vec4 vectors plus 16 sampler uniforms (224 conservatively), with 16 texture units.
The existing skin/vertex-graph minimum-device path remains unverified. Earlier browser checks are in Git history.

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
