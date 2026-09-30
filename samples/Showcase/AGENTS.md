# Working on this OwnEngine project (for AI agents)

A two-level coin collecting game: walk into the coins (W/A/S/D, Space jumps), clear level 1 to reach level 2, then "Play again".

- Scenes are plain JSON in `scenes/`, prefabs in `prefabs/`, Lua in `scripts/`, sounds in `sounds/`.
  You may edit files directly, but prefer the engine API so values are validated.
- Inspect: `oe exec . scene.summary` / `oe exec . component.types` / `oe exec . prefab.list`.
- Edit:    `oe exec . entity.create '{"name":"Box","components":{"MeshRenderer":{}}}' --save`.
           Spawn a coin: `oe exec . prefab.instantiate '{"path":"prefabs/coin.prefab.json","position":[2,0.6,-2]}' --save`.
- Verify:  `oe render . --out shot.png` then look at the PNG; `--frames 120` simulates 2 seconds first,
           `--colliders` draws collision shapes.
- Play-test headless: pipe commands into `oe script .`, e.g. input.key {key:"W"}, sim.step {frames:120},
           game.state (scene + score), audio.state (sounds played), render.screenshot, input.click {x,y}.
- Scripts: `oe exec . script.errors` lists Lua errors with file:line. Try code live with script.eval.
- Sounds:  `oe exec . audio.generate '{"path":"sounds/jump.wav","preset":"jump"}'` synthesizes effects.
- Live:    `oe mcp .` exposes every command as an MCP tool (screenshots come back as images).
- Human:   `oe editor .` opens the web editor on http://127.0.0.1:7777 (same API); `oe run .` plays in a window.
- Materials: `materials/*.mat.json` (chrome ball, glass pane, glowing cube) are PBR/transparency examples; edit with `material.set`, assign via `MeshRenderer.material`.
