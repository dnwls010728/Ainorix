# Working on this OwnEngine project (for AI agents)

Lantern Road, an HD-2D test game: pixel-art sprites turned to the camera by a script in a lit 3D world with a long lens and
depth of field. Walk with W/A/S/D, arrows or the left stick (Shift runs). Collect the five glowing crystal
shards, then talk to the Elder (E / Enter / gamepad A). Key 1 toggles all camera effects (Reinhard exposure,
bloom, vignette, depth of field), key 2 only depth of field. It demonstrates script billboards (`billboards.lua` on the `Billboards` entity: tag `billboard` = upright, `billboard-camera` = fully facing; the engine has no `Sprite.billboard`),
`Sprite.castShadows` (shadows shaped by the image) and `PostProcess` depth of field (`dofRadius`, `dofFocus`,
`dofRange`, `dofFalloff`). Scripts: `billboards.lua`, `hero.lua`, `game.lua`, `crystal.lua`, `flicker.lua`. `scenes/main.scene.json`
and `materials/*` are written by `python tools/make_scene.py` (edit the script and rerun it rather than the
scene when changing the layout). 1 world unit = 32 sprite pixels (`pixelsPerUnit: 32`).

- Scenes are plain JSON in `scenes/`, prefabs in `prefabs/`, Lua in `scripts/`, sounds in `sounds/`.
  You may edit files directly, but prefer the engine API so values are validated.
- Inspect: `oe exec . scene.summary` / `oe exec . component.types` / `oe exec . prefab.list`.
- Edit:    `oe exec . entity.create '{"name":"Box","components":{"MeshRenderer":{}}}' --save`.
- Verify:  `oe render . --out shot.png` then look at the PNG; `--frames 120` simulates 2 seconds first,
           `--colliders` draws collision shapes.
- Play-test headless: pipe commands into `oe script .`, e.g. input.key {key:"W"}, sim.step {frames:120},
           game.state (scene + score), audio.state (sounds played), render.screenshot, input.click {x,y}.
- Scripts: `oe exec . script.errors` lists Lua errors with file:line. Try code live with script.eval.
- Sounds:  `oe exec . audio.generate '{"path":"sounds/jump.wav","preset":"jump"}'` synthesizes effects.
- Live:    `oe mcp .` exposes every command as an MCP tool (screenshots come back as images).
- Human:   `oe editor .` opens the editor (it serves the same API on http://127.0.0.1:7777; attach with `oe mcp --connect 7777`); `oe run .` plays in a window.
- Materials: `materials/*.mat.json` (per-size block textures, `water.shader.json` graph) are written by `tools/make_scene.py`; edit with `material.set`, assign via `MeshRenderer.material`.
- Look:    `oe render . --frames 30 --out shot.png` (add `--renderer gpu` for the window's look); camera effects are the `PostProcess` component on the Camera.
- Art:     `assets/textures/*.png` came from AI image generation, reduced to exact sprite-sheet sizes; the prompts are in `tools/art_prompts.json`.
