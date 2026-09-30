# Working on this OwnEngine project (for AI agents)

"Crypt of Coins": a top-down 2D action game on Box2D physics. WASD/arrows walk, Space/J shoots a magic bolt
the way the hero faces, R restarts. Collect every coin, then take the stairs (E). Slimes chase you when they can
see you and take two bolts; touching one costs a heart (3 hearts).

- The level is the `Tilemap` on the `Level` entity (`scenes/main.scene.json`): one string per row, row 0 at the
  top, 1 tile = 1 unit. Its tile rules live in `tilesets/dungeon.tileset.json`: `#` wall (autotile `blob`,
  solid), `~` water (autotile `sides`, solid), `.` floor (random variants), `,` moss, `E` stairs (exit).
  Markers turned into objects by `scripts/level.lua` (the cell becomes floor): `P` player start, `c` coin,
  `s` slime, `b` crate, `o` pillar, `r` boulder. Edit the level by editing those strings (Tiles panel brush,
  `tilemap.paint`/`tilemap.fill`, `component.set`, or the JSON file); walls and water re-connect by themselves.
- Physics (all 2D): the hero and slimes are `CharacterBody2D {mode: "topdown"}` (layers 1 and 3), crates are
  `RigidBody2D` boxes with `gravityScale 0` + damping (the hero pushes them), bolts are `bullet` bodies on
  layer 2 that ignore layers 1 and 2, the boulder is a concave `Collider2D` polygon (the same points
  `tools/make_art.py` draws it from), pillars are circles, coins and the stairs are triggers.
- Scripts: `player.lua` (movement, shooting, hurt + knockback), `slime.lua` (wander, line-of-sight chase via
  `physics.raycast`, hits), `bolt.lua`, `coin.lua`, `exit.lua`, `game.lua` (hearts, coins, end screens),
  `level.lua`, `play_again.lua`. Prefabs: `coin`, `slime`, `bolt`, `crate`.
- Art: `tools/make_art.py` generates `assets/tiles/dungeon.png` (47 wall + 16 water autotile frames, floor
  variants, stairs) and `assets/sprites/*.png`. Edit and rerun `python tools/make_art.py`; `tools/` is not packaged.
- Verify: `oe render . --frames 30 --width 1280 --height 720 --out shot.png` (add `--colliders` for the
  physics outlines) and look at the PNG.
- Play-test headless (pipe into `oe script .` or call over MCP/HTTP):
  - `input.key {key:"D", down:true}`, `sim.step {frames:30}`, `input.key {key:"Space", down:true}` ...
  - state: `script.eval {code:"return {#scene.withTag('enemy'), #scene.withTag('coin')}"}`,
    `tilemap.info {id:"Level"}`, `physics.contacts {id:"Player"}`, `physics.state`
  - teleport for focused tests: `component.set {id:"Player", type:"Transform", values:{position:[x,y,0.1]}}`
- Human: `oe run .` plays in a window; `oe editor .` opens the editor (select Level, Paint Tiles to draw).
