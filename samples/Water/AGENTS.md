# Working on this OwnEngine project (for AI agents)

Rolling sea, a 3D test project for shader graph vertex displacement. The sea is one `plane64` mesh (the
plane split into 64x64 cells) scaled to 44x44. Its graph `materials/water.shader.json` sums four sine
waves, one wave per register channel: the `offset` output lifts every vertex, the `normal` output gives
each pixel the matching slope (the sun glitter comes from the ordinary PBR lighting) and the color goes
from the trough color to the crest color, with foam on high crests. Keys 1 / 2 / 3 switch the sea state
(Calm, Swell, Storm); A / D or the arrows orbit the camera, W / S zoom, Q / E change its height.

- `scripts/waves.lua` holds the wave table (direction, wavelength, amplitude) and `Waves.sample(x, z, t)`.
  `tools/make_scene.py` has the same table (`WAVES`) and writes the graph's default uniforms from it:
  change both together.
- `scripts/ocean.lua` (on `Water`) eases amplitudes and colors to the chosen sea state and sends them to
  the graph every frame through `MeshRenderer.shaderUniforms` (`amp`, `deep`, `shallow`).
- `scripts/float.lua` puts buoys, crates and the boat on the surface and leans them with the slope. It
  samples the waves at `time.now() + dt`: the frame is drawn with the time after the step.
- `scripts/orbit.lua` (on `Camera`) writes `CameraFollow.offset`.
- The displacement is render-only: physics and raycasts see the flat plane.
- `scenes/main.scene.json` and `materials/*` are written by `python tools/make_scene.py` (edit the script
  and rerun it rather than the scene when changing the layout or the graph).

- Scenes are plain JSON in `scenes/`, Lua in `scripts/`. You may edit files directly, but prefer the
  engine API so values are validated.
- Inspect: `oe exec . scene.summary` / `oe exec . component.types` / `oe exec . shader.check '{"path":"materials/water.shader.json"}'`.
- Verify:  `oe render . --frames 90 --out shot.png` then look at the PNG (add `--renderer gpu` for the window's look).
- Play-test headless: pipe commands into `oe script .`, e.g. input.key {key:"3"}, sim.step {frames:300},
           render.screenshot.
- Scripts: `oe exec . script.errors` lists Lua errors with file:line. Try code live with script.eval.
- Human:   `oe editor .` opens the editor; `oe run .` plays in a window.
