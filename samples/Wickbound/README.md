# Wickbound

A 10-minute survivor roguelite where light is your resource. Weapons fire on their own;
you steer, dodge and keep the lantern fed. Oil burns down every second, critters outside
the light are faster and hit harder, and you only see their eyes. Light braziers for
permanent safe zones, level up, evolve weapons from boss chests and outlast the Moon Muncher.

The game design, art and text come from a TypeScript + Canvas web game of the same name;
this version is built from the engine's own pieces rather than ported line by line.

Run `build/bin/oe.exe run samples/Wickbound` (add `--save-dir <dir>` to keep progress
between launches; packaged players save by default).

Controls: WASD / arrow keys, or drag anywhere (virtual joystick). Esc or P pauses. Level-up
choices: click or keys 1-3.

## How it is built

Two scenes: `scenes/menu.scene.json` (title, play setup, upgrades, achievements, shop,
settings, result) and `scenes/run.scene.json` (the arena, HUD and dialogs). `game.loadScene`
switches between them; `game.set("run" / "result")` carries the choice and the outcome.

| Thing | Engine pieces | Script |
|---|---|---|
| Keeper | `CharacterBody2D` (top-down); children: body sprite, additive lamp glow, `Light2D` lantern, Hurtbox and Magnet sensors (`Collider2D isTrigger` on kinematic bodies), one child entity per weapon | `run/player.lua`, `run/hurtbox.lua` |
| Weapons | Child entities of the keeper, instantiated from `prefabs/weapon.prefab.json`; flame-ring orbs are their children | `run/weapon.lua` |
| Critters | Ten prefabs: `RigidBody2D` + circle `Collider2D` on the enemy layer (they push each other apart), children for shadow, body, elite aura and the eyes drawn above the darkness | `run/enemy.lua` |
| Shots | Trigger colliders on kinematic bodies: `onTriggerEnter` damages the critter; bolts carry a trail `ParticleEmitter` | `run/projectile.lua`, `run/flask.lua`, `run/pool.lua` |
| Pickups | Kinematic bodies only the keeper's sensors see: Magnet pulls, Hurtbox collects | `run/pickup.lua` |
| Braziers | Static trigger ring, `Light2D` (dim until lit), `fx/spin.lua` on the dashed ring, spark emitter | `run/brazier.lua` |
| Arena | One `Tilemap` for the ground, an edge-loop `Collider2D` as the wall, `CameraFollow` | `run/camera.lua` (shake) |
| Darkness | `Darkness2D` on the camera; the lantern, lit braziers, chests and bosses are `Light2D` | — |
| Rules | Time, XP and levels, the build, chests, death, victory | `run/game.lua`, `run/director.lua` (spawning) |
| HUD and dialogs | UI entity trees with `UILayout`; dialogs sit on `UIPanel {blockInput}` backdrops and pop in with `UIMotion` (cards staggered by `delay`); `game.pause` freezes the world while one is open | `run/hud.lua`, `run/dialogs.lua`, `ui/button.lua` |
| Menu | Screen roots toggled by visibility, each fading in with `UIMotion`; the achievements list is a `UIScroll` view; buttons use `hoverScale` | `menu/menu.lua` |
| Effects | Prefabs with additive sprites / emitters that remove themselves | `fx/burst.lua`, `fx/ring.lua`, `fx/beam.lua` |

Shared modules in `scripts/lib/`: `defs` and `balance` (content and numbers; distances are
world units, 1 unit = 40 design pixels), `world` (who is in the run: enemy registry,
spawning prefabs, area queries through `physics.overlapSphere`), `meta` (save data, upgrades,
achievements, daily gift on `time.date`), `audio`, `i18n`, `ui`, `ads` (mock overlays).

Collision layers: 0 wall, 1 keeper, 2 critters, 3 shots, 4 enemy bullets, 5 pickups,
6 brazier zones, 7 Hurtbox, 8 Magnet. Sorting layers (`Sprite.layer`): ground 0 up to
particles 12, the darkness at 20, eyes and indicators above it.

`tools/make_content.py` writes the prefabs and both scenes (they are ordinary engine files
and can be edited in the editor - the stopped Game view moves and resizes the UI, the Hierarchy eye shows hidden screens, and texts carry
their English wording at edit time - but rerunning the tool overwrites them). `tools/make_art.py`
draws the effect textures and `tools/make_audio.py` renders the sounds.

Runtime character art uses sprite-gen animation atlases in `assets/sprites/animated/`:
three keepers, seven critters (gloomlet shares gloom), two bosses and one campfire.
Seven assets now use reviewed Grok video loops: Ada/Bram, Brute, Shade/Wisp,
King/Eclipse. Idle/hover has 64 frames over three seconds; walking has 16/19/31/18
frames at the generated native timing. The six rejected assets (Suri, Gloom,
Skitter, Stalker, Leech and Brazier) use their original single images without
SpriteAnimation; their installed sheets and sidecars have been removed. Gloomlet
shares Gloom's single image. See [Grok motion correction](tools/GROK_MOTION.md).
`SpriteAnimation` selects movement while moving and idle while stopped/stunned
for animated assets; game pause freezes playback. Campfire ignition keeps the
bowl static and enables its existing fire sprites, particles and light.
Menu portraits keep the original still images.

`animated/animations.json` contains engine clips derived from sprite-gen's
`manifest.json.frame_layout`, with each source manifest and QA notes beside its PNG.
`tools/generate_sprites.py` prepares, generates, extracts and installs reviewed runs;
`tools/verify_sprites.py` checks all keeper selections and captures both renderers.
See [sprite work log](tools/SPRITES.md) for reproduction and validation details.
Humanoid movement remains experimental; physical foot-contact cycles are not
certified. No engine/runtime rebuild is needed for these asset and Lua changes.

## What is in it

Eight weapons with evolutions, eight passives, seven enemy types, two bosses, six braziers,
three keepers, three stages, nine permanent upgrades, thirteen achievements, a daily gift,
a mock shop and mock rewarded/interstitial "ads" (demo overlays, nothing is charged or
shown), English and Korean text, settings and save data.

## Not done

- No portrait layout and no pause when the window loses focus.
- The web and Android players in `runtime/` predate the engine features this sample uses
  (`Light2D`, sorting layers, `game.pause`, ...) and must be rebuilt before
  `oe package --web` / `--android` can run it.
- Not yet played start to finish by a person; balance follows the original's numbers but
  critters now collide physically, so crowds behave differently from the web game.

## Assets and licenses

- `assets/sprites/`, `assets/ui/keyart.jpg`: the original game's art.
- `assets/sprites/animated/`: seven reviewed Grok video-derived animation sets
  generated from the original references, extracted and composed with
  [sprite-gen](https://github.com/aldegad/sprite-gen).
- `assets/fx/`: generated by `tools/make_art.py`.
- `sounds/`: rendered by `tools/make_audio.py` from the original's synthesizer definitions.
- `assets/fonts/Jua-Regular.ttf`: Jua, SIL Open Font License 1.1 (`assets/fonts/OFL-Jua.txt`).
- `assets/icons/`: Noto Emoji images, Apache License 2.0 (github.com/googlefonts/noto-emoji).
