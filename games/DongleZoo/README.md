# Dongle Zoo

A portrait merge puzzle for phones: drag to aim, let go to drop a round animal into the jar;
two of a kind merge into the next bigger one (chick → hamster → ... → whale). The game ends
when an animal rests above the dashed line. Design, monetization and the work log are in
[docs/DONGLEZOO.md](../../docs/DONGLEZOO.md) (Korean).

```bat
build\bin\oe.exe run games/DongleZoo --width 540 --height 960
build\bin\oe.exe package games/DongleZoo --web       :: dist/DongleZoo-web, test with `oe serve`
build\bin\oe.exe package games/DongleZoo --android   :: dist/DongleZoo-android/DongleZoo.apk
```

Controls: mouse or one finger. Esc (Android: Back) pauses or closes a dialog.

## How it is built

One scene (`scenes/main.scene.json`) and one script entity, `App`.

| Thing | Engine pieces | Script |
|---|---|---|
| Animals | Eleven prefabs `prefabs/piece_NN.prefab.json`: `RigidBody2D` + circle `Collider2D`, a `Face` sprite child | `lib/board.lua` (spawn, merge on `physics.contacts`, danger line, items) |
| Jar | Three static box `Collider2D`s, a glass sprite, the dashed line sprite | — |
| Aiming | `input.mouse()` mapped to world units; a `Holder` sprite and a dotted `Guide` | `app.lua` (`updatePlay`) |
| Screen shapes | `input.viewSize()` sets the camera `orthoSize` and `UICanvas.match`, so the jar fills the width on phones and the height on wide screens (96 reference px per world unit everywhere) | `app.lua` (`layout`) |
| Effects | `fx_burst` (`ParticleEmitter`, tinted per animal) and `fx_ring` prefabs, `tween` for pops and floating score texts | `app.lua` (`burst`, `popText`) |
| Screens | UI trees toggled by visibility: `Home`, `HUD`, eight `Dlg:*` dialogs on `UIPanel {blockInput}` backdrops with `UIMotion`; `game.pause` while a dialog is open during play | `app.lua`, `ui/button.lua` |
| Progress | Coins, items, backgrounds, collection, best score, daily gift (`time.date`), three daily missions | `lib/meta.lua` (`save.get/set/flush`) |
| Ads and store | `host.call` / `host.poll` to the embedding app, demo overlays without one | `lib/ads.lua`, `web/host.js` |
| Text | English and Korean; UI elements tagged `t:<key>` | `lib/i18n.lua`, `lib/ui.lua` |

Numbers (sizes, scores, prices, mission goals) are in `scripts/lib/defs.lua`.
Sprite sorting layers: background 0, jar 1, guide 2, line 3, animals 5, held animal 6, effects 8.

## Tools

Run from this folder; `tools/` and `web/` are not part of the game data.

- `tools/make_art.py` — backgrounds, jar, effect textures, UI icons and a drawn set of animals.
- `tools/import_art.py` — installs the illustrated animals and the app icon from `tools/art_src/codex/`
  (run it after `make_art.py`; it fits each picture's round body to the physics circle).
- `tools/make_audio.py` — sound effects and the music loop.
- `tools/make_content.py` — the prefabs and the scene. They are ordinary engine files and open in the
  editor, but rerunning the tool overwrites them.

## Not done

See the unchecked lines in [docs/DONGLEZOO.md](../../docs/DONGLEZOO.md): real ads need an AdSense
publisher id on the web and a Java layer on Android; the APK and App Bundle build and verify but
have not been run on a device or emulator.

## Assets and licenses

- `assets/animals/`, `assets/icon.png`: generated for this game with Codex image generation.
- `assets/bg/`, `assets/fx/`, `assets/ui/`: drawn by `tools/make_art.py`. `sounds/`: `tools/make_audio.py`.
- `assets/fonts/Jua-Regular.ttf`: Jua, SIL Open Font License 1.1 (`assets/fonts/OFL-Jua.txt`).
