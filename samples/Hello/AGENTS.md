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
- Saves: `save.state` shows the high score. Lua `save.get/set/flush` keeps the best total across Play Again; tools use memory unless `--save-dir <dir>` is supplied, packaged players use platform storage.
- Sounds:  `oe exec . audio.generate '{"path":"sounds/jump.wav","preset":"jump"}'` synthesizes effects.
- Live:    `oe mcp .` exposes every command as an MCP tool (screenshots come back as images).
- Human:   `oe editor .` opens the editor (it serves the same API on http://127.0.0.1:7777; attach with `oe mcp --connect 7777`); `oe run .` plays in a window.

- Common edits: see `EDITING.md`. Player `Script.params` speed/jumpSpeed/gravity keep
  literal defaults for inspector and script.params inference; stages/UI remain scene data.
- Player code uses `require("oe.input")` with an explicit offline `localSource()` and
  named move/jump bindings, then `require("oe.math").normalize` for XZ direction.
  Do not use playerSource unless intentionally converting this project to network play.
- HUD uses `require("oe.ui")`: `widgets.find("Message", "UIText")`, `widget:text(...)`,
  `widget:set({...})`, `widget:show()`. Select the component explicitly; optional widgets
  use tryFind. Handles belong to this scene/session and never silently re-find replacements.
- Official modules are embedded; do not copy them into this project or shadow oe.*.
  Apply project helper edits with script.reload; failures retain previous code. Keep engine
  calls out of module initializers. Instance fields/computed labels keep their current values;
  restart Play when onStart needs to run again.
