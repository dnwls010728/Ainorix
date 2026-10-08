# Editing this game

This is an offline, two-level coin game. Edit the matching location, then verify the
gameplay flow; no shop, merge rule or dialog framework is part of this template.

| Change | Location | Verify |
|---|---|---|
| Player speed | Player's `Script.params.speed` in each `scenes/*.scene.json`; default 4 units/s in `scripts/player_controller.lua:onStart` | Hold W/D; diagonals stay the same speed. Use the inspector or component.set with `--save`. |
| Jump speed | `Script.params.jumpSpeed`, default 5 units/s | Press/hold Space, land again. |
| Gravity | Without CharacterBody: `Script.params.gravity`, default 12 units/s². With CharacterBody (both supplied levels): `CharacterBody.gravityScale`, default 1, multiplies native gravity 9.81 units/s² | Jump/fall/land; the physics path uses the body's gravityScale, not the fallback parameter. |
| Score awarded per coin | `scripts/game_manager.lua:onCoinCollected`, the `game.set("total", ... + 1)` expression | Walk through coins; total and saved highScore update. `collected` remains the number of coins, not awarded points. |
| Coins needed / stage contents | Coin entities tagged `coin` in `scenes/main.scene.json` or `level2.scene.json`; spawn with `prefabs/coin.prefab.json` | Collect every coin; the game counts tagged coins at startup. There is no separate duplicated goal count. |
| Coin appearance/body | `prefabs/coin.prefab.json` for new instances; existing level entities store their own component values | Render and check trigger collection. Changing the prefab alone does not rewrite existing instances. |
| HUD/win text | Dynamic score/clear/win strings: `scripts/game_manager.lua:refresh/onCoinCollected`; Play Again/level title labels: scene UI components | Render the HUD, finish both levels, click Play Again. This template has English literals, no localization module. |
| UI width/position | UIText/UIPanel/UIButton components in each scene | `ui.layout`, render at the target view size; widgets change only the selected component. |
| Next level | Game Manager's `Script.params.nextScene`; `""` means final level | Level clear waits 1.5 seconds, loads nextScene; final level shows Play Again. |
| Movement key bindings | Named `actions` in `scripts/player_controller.lua` | Inject WASD/arrows/Space; opposed keys cancel. No gamepad axes/buttons are bound by default. |

Examples from the project directory:

```text
oe exec . script.params '{"path":"scripts/player_controller.lua"}'
oe exec . component.set '{"id":"Player","type":"Script","values":{"params":{"speed":6}}}' --save
oe exec . script.check '{"path":"scripts/player_controller.lua"}'
oe render . --frames 120 --out shot.png
```

Component partial updates preserve fields of a component, but a supplied nested params
object replaces that object: include any other parameter overrides you want to keep.
Set speed separately in both levels when the whole game should change. To drive movement,
use `oe script .`: `input.key {"key":"W"}`, `sim.step {"frames":110}`, `input.clear {}`.
Verify `game.state`, `save.state`, `audio.state`, `script.errors` and screenshots.

`oe.input.localSource()` is the explicit offline input source. `oe.math.normalize`
maps the named-coordinate XZ wish vector to unit length or zero. `oe.ui` supplies
capability-checked widget handles; acquire them on the scene's owner instance at startup.
Official modules are embedded in the player; no project copy is needed. Keep old scene.*
operations when they are already clearer (coin tags, physics, player respawn).

All template scene/script/prefab files are authored project files; there is no scene
generator inside this game. Repository maintainers edit `templates/default/` and regenerate
`tests/fixtures/Hello` (its test copy) using `oe new`; do not maintain two independent template copies.
`oe new` synthesizes sounds and optionally seeds an agent team. Apply project helper edits
with `script.reload`; changed required modules and their dependent attached scripts reload
without restarting Play. Keep module initializers free of engine calls (including queries);
a failed reload retains the previous code. Existing instance fields, computed values and
translated labels are not automatically refreshed; restart Play when startup must run again.
