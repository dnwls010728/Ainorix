# Working on this OwnEngine project (for AI agents)

"Target Range": a first-person shooter test game. Mouse look, WASD move, Shift sprint, Space jump,
click to shoot, R to reload, Escape frees the mouse (click to capture it again). Shoot all 10 targets;
the results screen shows the time and accuracy, "Play again" restarts.

- Scene: `scenes/main.scene.json`. Player = `Player` (CharacterBody) with children `Head` (Camera),
  `Head/Gun`, `Head/MuzzleFlash`. Targets are tagged `target`; UI entities: TargetsText, TimeText,
  AmmoText, Crosshair, ResultPanel, ResultText, PlayAgain.
- Scripts: `scripts/fps_player.lua` (look, move, gun), `scripts/target.lua` (static / strafe / bob,
  health), `scripts/game_manager.lua` (count, clock, results), `scripts/play_again.lua`.
- Inspect: `oe exec . scene.summary` / `oe exec . component.types`.
- Verify:  `oe render . --out shot.png` then look at the PNG; `--frames 60` simulates 1 second first.
- Play-test headless (pipe into `oe script .` or call over MCP/HTTP):
  - look:  `input.mouse {dx, dy}` (pixels; 0.12 degrees per pixel), then `sim.step {frames:1}`
  - shoot: `input.mouse {button:"MouseLeft", down:true}`, `sim.step {frames:1}`, then release
  - state: `script.eval {code:"{self.yaw, self.pitch, self.ammo}", entity:"Player"}`,
           `script.eval {code:"#scene.withTag('target')"}`, `game.get('shots')` / `game.get('hits')`
- Scripts: `oe exec . script.errors` lists Lua errors with file:line.
- Human:   `oe run .` plays in a window; `oe editor .` opens the web editor (Game view captures the mouse).
