# games/RPG — HD-2D village port

`games/RPG` is the OwnEngine version of the three.js prototype in `E:\Projects\RPG` (Vite + TypeScript, every texture
drawn on canvases at start-up). The goal was to rebuild it with engine features, extend the engine where it could not
express the prototype, and replace every procedural picture with newly generated art.

## What maps to what

| Prototype (three.js) | Engine |
|---|---|
| `world.ts` voxel terrain (instanced boxes per tile/height) | `tools/make_world.py` ports the mulberry32 rng and value noise exactly, writes only the visible faces into `assets/models/terrain.glb` (one 6-cell atlas, per-tile tint in `COLOR_0`) |
| `applyFx` cloud shadows / grass sway / hero push (patched built-in shaders) | surface graphs `materials/ground.shader.json` (fragment) and `plants.shader.json` (vertex `offset`, uniform `hero` set from Lua) |
| `house.ts` half-timber houses | `tools/make_houses.py`: one model per house (`house_N.glb`, entity `HouseN`) with stone plinth, modelled timber frame (posts, beams, studs around openings, braces), gable roof with thickness, eaves, rake, fascia, soffit and ridge cap (ridge along the longer side), gables with king post, framed windows with sills and tinted shutters, framed door with step and hood, capped chimney; per-house roof/plaster/shutter colours, one two-storey jettied house. Glass in `house_N_windows.glb` (child `Windows`, tag `windows`) with `window.shader.json` (`glow` uniform) |
| water `ShaderMaterial` with shore-distance texture | `water.shader.json` on a 700 m plane, `shore.png` (4-texel deep border, clamped), unlit, tinted by a `light` uniform |
| `sky.ts` dome (gradient, sun, clouds, stars, moon) + 3 ridge cylinders | `sky.shader.json` (gradient, sun disc/halo, stars from `stars.png`), a separate `clouds.shader.json` dome (48-node graph limit), `mountain_ring.glb` x3, materials with `fog: false` |
| `Billboard` (Y-axis sprites, blob shadow, emissive 0.22, tree sway) | `Sprite {lit, castShadows, emission: 0.22}` + child blob `Sprite`, turned by `game.lua` (tag `billboard`); trees add `material: materials/tree.mat.json` (sway graph) |
| `Particles` (motes/fireflies, chimney smoke, footstep dust), `LightShafts` | `effects/motes|smoke|dust.vfx.json` (`vfx.set` colour/size, `vfx.burst`), seven additive shaft sprites oriented along the sun ray |
| `post.ts` bloom, tilt-shift bokeh, grade (split tone, saturation, contrast, vignette), ACES | `PostProcess`: bloom, depth-based DOF, `toneMapping: aces`, `saturation`, `contrast`, `shadowTint`, `highlightTint`, vignette |
| `THREE.Fog` sharing the haze colour | `PostProcess.fog/fogColor/fogStart/fogEnd` (per surface) |
| `HemisphereLight` + `DirectionalLight` | `DirectionalLight {hemisphere, groundAmbient}` |
| `atmosphere.ts` keyframes | `scripts/lib/atmosphere.lua` (same keys and smoothstep) |
| `main.ts` movement, camera, intro, HUD | `scripts/hero.lua`, `scripts/game.lua`, UI entities (`NanumMyeongjo-Bold.ttf`) |

Units follow the prototype: one tile = one unit, block tops at integer heights, character sprites 32x48 px at 28 px
per unit, trees 20 px per unit. Light intensities are divided by pi (engine light colours include pi).

The banner title in the prototype's `index.html` was stored with a broken encoding (only "...더미어 광장" survives);
the port names the plaza "윈더미어 광장".

## Engine changes made for it

- `PostProcess.fog`, `fogColor`, `fogStart`, `fogEnd` (per-surface distance fog, additive surfaces fade) and
  material `fog: false`.
- `PostProcess.saturation`, `contrast`, `shadowTint`, `highlightTint` and `toneMapping: "aces"`.
- `DirectionalLight.hemisphere` + `groundAmbient`.
- `Sprite.emission` and `Sprite.material`.
- glTF `COLOR_0` vertex colours.
- Game key names `LeftBracket` / `RightBracket` (Windows, web, Android).

All defaults keep earlier frames bit-identical; `FogGradingHemisphereAndSpriteEmission` covers them in both renderers.

## Art

`tools/art_prompts.py` -> `tools/art_prompts.json` (29 prompts: 4 character sheets, 6 trees, 2 bushes, rock, plants
strip, lamp, 6 terrain textures, plaster/roof/door/window, 2 mountain strips, clouds, moon). `tools/gen_art.py` runs one
`codex exec` per prompt in a read-only sandbox (the Windows workspace sandbox of the Codex plugin failed with
"helper_unknown_error: setup refresh had errors", so Codex only calls its image tool and the script copies the result
from `~/.codex/generated_images/`). `tools/import_art.py` crops figures by their alpha, reduces them with
coverage-weighted averages, snaps them to a small palette and adds a tinted outline. Technical textures (glow, blob
shadow, shore distance, shaft gradient, star field) and the atlases are computed by `make_world.py`.

## Work log

- [x] Engine: fog, grading, ACES, hemisphere ambient, sprite emission, COLOR_0, bracket keys; shaders regenerated
      (sokol-shdc); test `FogGradingHemisphereAndSpriteEmission`.
- [x] Art generated with Codex and imported.
- [x] World generator, scene, materials, graphs, effects; Lua director and hero.
- [x] Test `RPGVillageGame`; D3D11 screenshots at dawn, noon, golden hour, dusk, night and the opening shot inspected.
- [x] Web and Android players rebuilt (see runtime READMEs).
- [x] Houses rebuilt as separate, detailed models (`tools/make_houses.py`).
- [x] Tree sway: engine `Sprite.material`; trees use `materials/tree.shader.json` (crown offset, slow world-space phase).
- [x] Moon: `Moon` + `MoonGlow` sprites (Codex `moon.png`, `materials/moon.mat.json` with `fog: false`) placed by
      `game.lua` 160 m along the prototype's moon direction, faded in with the stars. The sky graph has no room for it
      (46 of 48 nodes).
- [ ] Not ported: the prototype's chromatic aberration and film grain, true tilt-shift (DOF is depth based).
- [ ] Not run: browser GPU, Android device, Linux/EGL.
