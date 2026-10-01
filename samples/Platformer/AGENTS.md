# Working on this OwnEngine project (for AI agents)

"Pixel Meadow": a 2D side-scrolling platformer. Arrows/A-D move, Space/W jump (hold for higher), R restarts.
Collect coins, bump "?" blocks from below, stomp slimes, avoid spikes and pits, reach the flag. 3 lives.

- The level is the `Tilemap` on the `Level` entity (`scenes/main.scene.json`): one string per row, row 0 at
  the top, 1 tile = 1 unit. Legend: `#` grass, `D` dirt, `B` brick, `P` planks, `X` stone, `?` bonus block,
  `u` used block (solid); `^` spikes, `b f t` bush/flower/grass, `| F` flag pole (decoration).
  Markers turned into objects by `scripts/level.lua`: `S` player start, `c` coin, `e` slime, `G` goal.
  Edit the level by editing those strings (inspector, `component.set`, or the JSON file).
- Scripts: `player.lua` (movement feel: acceleration, coyote time, jump buffer, variable jump; hazards; head
  bumps), `slime.lua` (patrol, stomp), `game.lua` (lives, coins, clock, end screens), `level.lua`,
  `coin.lua`, `pop.lua`, `goal.lua`, `parallax.lua` (background layers tagged `parallax hills/clouds`).
- Prefabs: `prefabs/coin.prefab.json`, `prefabs/slime.prefab.json`.
- Effects: `scripts/effects.lua` exports `spawn(position, "coin"|"hit")` for coin,
  bonus-block, player-damage and stomp bursts. Separate world-space emitters use
  0.5-second particles and destroy their effect entities after 0.65 seconds.
- Art: `tools/make_art.py` generates `assets/sprites/*.png` (player 9 frames, tiles 8x2, coin 4, slime 3,
  hills, cloud). Edit and rerun `python tools/make_art.py`; `tools/` is not packaged.
- Verify:  `oe render . --frames 60 --out shot.png` (1280x720 recommended) and look at the PNG.
- Play-test headless (pipe into `oe script .` or call over MCP/HTTP):
  - `input.key {key:"Right", down:true}`, `sim.step {frames:30}`, `input.key {key:"Space", down:true}` ...
  - state: `script.eval {code:"local p=self:position() return {p.x, p.y, self:grounded()}", entity:"Player"}`,
    `script.eval {code:"game.get('coins')"}`, `script.eval {code:"tilemap.get('Level', 12, 8)"}`
  - teleport for focused tests: `component.set {id:"Player", type:"Transform", values:{position:[x,y,0]}}`
- Human: `oe run .` plays in a window; `oe editor .` opens the editor (View > 2D Scene View for this level).
