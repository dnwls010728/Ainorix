# Working on this OwnEngine project (for AI agents)

"윈더미어 광장" (Chapter I), an HD-2D village: pixel-art billboards (hero, villagers, trees, lamps) in a lit voxel
world with a day/night cycle, fog, bloom, colour grading and depth of field. It is the OwnEngine port of a three.js
web prototype (`E:\Projects\RPG`); the terrain generator keeps the prototype's seeded noise, so the map is the same.

Controls: W/A/S/D or arrows (left stick) walk, Shift runs, Q/E (right stick) turn the camera 45 degrees, mouse wheel
zooms, R/F tilt, T stops/starts time, `[` / `]` scrub the time of day.

- `scripts/game.lua` (entity `Game`): time of day (`scripts/lib/atmosphere.lua` keyframes) applied to the `Sun`
  (direction, colour, hemisphere ambient), the camera's `PostProcess` (exposure, ACES, bloom, vignette, fog, saturation,
  split toning, depth of field), sky/cloud graph uniforms, mountain tints, window glow, lamp halos/lights, sun shafts,
  motes and smoke; the orbit camera and its 5.6 s opening shot; billboards (tag `billboard`) turned to the camera; HUD.
  Params: `startTime` (0 dawn, 0.25 noon, 0.5 golden hour, 0.68 dusk, 0.85 night), `daySeconds`, `skipIntro`.
- `scripts/hero.lua`: grid movement over `scripts/lib/terrain.lua` (generated heights, `#` = blocked), step up/down one
  block, 4x3 sheet animation (rows down/right/up, left = mirrored), footstep dust bursts.
- Generated content: `python tools/make_world.py` writes `assets/models/*.glb` (houses come from
  `tools/make_houses.py`, one model per house; colours/storeys in `HOUSES` of make_world.py), the atlases and technical textures
  (glow, blob shadow, shore map, shaft, stars), `materials/*` (materials + shader graphs), `effects/*.vfx.json`,
  `scripts/lib/terrain.lua` and `scenes/main.scene.json`. Edit the script and rerun it rather than the generated files.
- Art: every picture came from Codex image generation. `tools/art_prompts.py` writes the prompts,
  `tools/gen_art.py` runs Codex (`codex exec`, read-only sandbox, image tool only) into `tools/art_src/codex/`,
  `tools/import_art.py` reduces them to real pixel sizes in `assets/textures/` (sheets 4x3 cells of 32x48).
- Font: `assets/fonts/NanumMyeongjo-Bold.ttf` (SIL OFL, `OFL-NanumMyeongjo.txt`).

Checks:
- `oe exec . script.check '{"path":"scripts/game.lua"}'`, `oe exec . script.errors`.
- Screenshots: pipe `component.set {id:"Game",type:"Script",values:{params:{startTime:0.85,skipIntro:true,daySeconds:320}}}`,
  `sim.step {frames:300}`, `render.screenshot {renderer:"gpu",path:"shot.png"}` into `oe script .`.
- The engine test `RPGVillageGame` plays it headless (intro lock, walking, camera turn, night lamps and clock).
- Human: `oe run .` plays it in a window; `oe editor .` opens the editor.
