# Editing Dongle Zoo

Run commands from the repository root. Start with the location below; the engine API
and unrelated game systems do not need to change for these edits.

| Edit | Authored location | Steps and verification |
|---|---|---|
| Player movement speed | N/A | This is a pointer-controlled drop puzzle, with no moving player. Do not substitute another task when measuring player-speed edits. |
| Drop cooldown (a separate supported edit) | `scripts/lib/defs.lua`, `D.DROP_COOLDOWN` | Change the scalar, script.reload, drop twice. Gravity/body tuning instead belongs to `tools/make_content.py`, `prefabs()`, and requires prefab regeneration. |
| Merge combo scoring | `scripts/app/scoring.lua`, `mergePoints(base, combo)` | Edit only the multiplier/cap and rounding rule. Base animal scores remain in `scripts/lib/defs.lua`, `D.TIERS`; do not edit missions/effects. Verify two close merges and the HUD score. |
| Resume label in EN/KO | `scripts/lib/i18n.lua`, `pause.resume` | Edit both strings in this one row. Restart and open Pause in both languages. Generate the scene too if the stopped editor should display the new English label. Runtime localization replaces scene text via its `t:pause.resume` tag. |
| Add a coin-pack shop product | `config/shop.json`, `products` | Copy a coin pack; give it a unique lowercase ID, positive integer `coins`, display `price`, and EN/KO `name`/`desc`. Run scene-only generation below. Open Shop, scroll to the row, buy via the demo and check coins. No manual row/action/entity-name edit is needed. |
| Pause popup width | `config/layout.json`, `pause.width` | Change reference-pixel width (720x1280 design), generate, then render Pause. Do not edit generated UIPanel JSON. |
| Pause popup title in EN/KO | `scripts/lib/i18n.lua`, `pause.title` | Change both strings and generate for stopped-editor English text. Open Pause in both languages; width and title are two distinct authored locations. No projection/font approximation is involved. |
| Gift/mission/item values | `scripts/lib/defs.lua` | Edit the focused reward/mission/item table; verify the corresponding flow. Existing save keys and service code stay unchanged. |

Scene/data generation (preserves prefabs):

```text
python games/DongleZoo/tools/make_content.py --scene-only
build/bin/oe exec games/DongleZoo script.check '{"path":"scripts/app.lua"}'
build/bin/oe render games/DongleZoo --frames 3 --out build/zoo-menu.png
build/bin/oe_tests
```

For a reproducible interactive check, use `oe script games/DongleZoo`, set
`time.date {"set":"2031-03-03"}`, step three frames, and drive named buttons via
`ui.layout` + `input.click`. For Pause, start Play and press `HUD:pause`. For a shop
purchase, open `Home:btn:shop`, click `Product:<id>:btn`, then `Ad:buy` in the demo.
`save.state` contains `donglezoo.save.v1`; `host.state` shows `iap.purchase` and the SKU.
The demo grants coins without charging; real store products still require external SKU
provisioning. `D.DEMO_STORE` in defs.lua controls the existing fallback policy.

## Ownership and file boundaries

`scripts/app.lua` is the sole attached App script: startup, frame order and dispatch.
It explicitly supplies services on the instance. `scripts/app/gameplay.lua` implements
drop/merge/items/danger; `screens.lua` owns layout/HUD/home/dialog methods;
`rewards.lua` implements finalization/gifts/store/ad actions; `effects.lua` owns visual
lifetimes; `scoring.lua` is pure. These modules have no mutable module state or sibling
imports. The App owns run state; existing `lib/board.lua` owns pieces/order and
`lib/meta.lua` owns persistent coins/items/settings. Existing save/audio/i18n/ads/UI
modules retain their original contracts.

| Authored source | Generated output / overwrite rule |
|---|---|
| `config/shop.json` | `scripts/config/shop.lua` and shop rows in `scenes/main.scene.json`; never hand-edit either output. No Python parser for Lua is involved. |
| `config/layout.json`, i18n strings and `tools/make_content.py` | `scenes/main.scene.json`; all direct scene edits are overwritten by regeneration. Three products retain the original layout; more products use an existing UIScroll viewport. |
| `tools/make_content.py`, `prefabs()` | `prefabs/piece_*.prefab.json`, `fx_burst` and `fx_ring`; full generation overwrites them, `--scene-only` does not. Geometry shared with defs.lua still needs coordinated editing; M4 unifies products, not every geometry constant. |
| `tools/make_art.py` | Procedural textures; overwrites its asset outputs. Do not run for a label/product/rule edit. |
| `tools/art_src/codex/` + `tools/import_art.py` | Illustrated animals/icon; run after make_art to replace its fallback animal art. |
| `tools/make_audio.py` | Sound/music assets; do not run for these editing tasks. |
| `scripts/app/*.lua`, `scripts/lib/*.lua`, `scripts/ui/button.lua` | Hand-authored except the explicitly generated `scripts/config/shop.lua`. |

Ship both generated shop Lua and scene after a catalog edit. Adding a coin pack needs
generation but no manual scene work; new reward types or a different layout/art are
separate authored-code tasks. Check long product names/EN/KO text visually after adding
them. Do not edit `scripts/config/shop.lua` as a second product source.

`Script.params` is for per-instance button action/argument/target and optional App locale;
`ui/button.lua` keeps its literal target fallback so `script.params` inference works.
Shared balance/catalog/layout data is not a per-instance parameter. Labels stay in the
existing i18n table; only selected edit locations are moved to config.

## Reload and snapshots

Attached app.lua and required project helpers reload with `script.reload`, retaining
instance fields without rerunning onStart. Edits to stateless app/*.lua, defs and generated
shop Lua are watched; table identities in self.services are preserved. Inspect reloaded,
required, failed and skipped in the result, then script.errors. A failed batch keeps old code.
The live scoring pilot changed the combo factor 0.25 -> 0.50: points(10,2) changed 13 -> 15,
actual onMerge gain 125 -> 150, with score/context/fields preserved; source was restored.

Affected helper-local state resets. Editing stateful board/meta/audio/ads/UI modules may
require Stop/Play or an explicit owner reset; existing timer/RPC closures are not replaced.
Translation reload resets i18n's private locale to English. To retain the chosen language,
after reload run script.eval on App with
`self.services.I.setLocale(self.services.Meta.data.settings.locale); self:localize()`;
refresh/reopen the relevant screen for dynamically computed labels. Already computed
instance values and existing physics/UI scene structure do not update automatically.
Generated shop scene rows still require regeneration/scene reload as described above.
Keep module initializers free of engine calls/timer registration; reload rejects them.

For recording, enable `sim.record_state` before Lua execution/physics creation, then
use `sim.save_state`/`sim.load_state`. Project resources must remain immutable while
recording, so stop recording before edits/reload. The service context, required-module
cache and run state restore together. Old version-1 saves continue to load.
