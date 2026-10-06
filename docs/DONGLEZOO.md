# Dongle Zoo (동글 동물원) — game design document

Design, monetization and release notes for the casual mobile merge puzzle implemented in `games/DongleZoo`.
How it is built: [games/DongleZoo/README.md](../games/DongleZoo/README.md). Status: the work log at the end.

## 1. In one sentence

A portrait physics puzzle: drop round animals into a jar, and two of a kind that touch merge into the next larger
animal. The goal is to grow from a chick to a whale.

## 2. Why this game (market and revenue reasoning)

| Criterion | Judgment |
|---|---|
| All ages | One-sentence rule (merge two of a kind), playable without reading, no violence, competition or time pressure, one-handed |
| Proven genre | "Suika game" style drop-merge games are a long-running hit as a genre. Short sessions (3–8 minutes) and a strong reason to replay |
| Fit for monetization | The "just one more" moment right before game over matches rewarded ads naturally. Works on ads alone, without forced purchases |
| Production cost | Small content: 11 animals + 3 backgrounds. Physics (Box2D), UI and saves are already in the engine |
| What sets it apart | Animal characters with faces instead of fruit (a collection to fill), daily missions and a login reward to bring players back, three items that rescue a stuck board |

## 3. Audience and platforms

- Audience: all ages (controllable from about age 4). Core group: casual players in their teens to forties, parents
  playing with children.
- Platforms: Android (portrait only), web (H5 portals and an own page). The Windows build is for development and checks.
- Screen: designed for 720×1280. Narrow screens (9:19.5 and similar) get more empty space above the jar; wide ones
  (tablets, PC) show background at the sides. At every ratio the jar fills the screen width (`layout` in `scripts/app/screens.lua`).

## 4. Core play

1. Hold and drag left/right to aim; release to drop the animal.
2. Two of the same animal that touch merge into the next stage and give points.
3. The next animal is shown in the "Next" box at the top right (weighted random among stages 1–5).
4. The game ends when an animal stays above the red dashed line for 2 seconds.

### The 11 animals

| Stage | Animal | Radius (units) | Points when made |
|---|---|---|---|
| 1 | Chick | 0.26 | – |
| 2 | Hamster | 0.34 | 1 |
| 3 | Rabbit | 0.45 | 3 |
| 4 | Cat | 0.52 | 6 |
| 5 | Dog | 0.66 | 10 |
| 6 | Fox | 0.80 | 15 |
| 7 | Panda | 0.92 | 21 |
| 8 | Bear | 1.08 | 28 |
| 9 | Lion | 1.25 | 36 |
| 10 | Elephant | 1.43 | 45 |
| 11 | Whale | 1.62 | 55 |

- Two whales merging both disappear for +100 (a "whale party").
- Combo: merges within 1 second of each other multiply points by (1 + 0.25 × (combo − 1)), up to +150%.
- Jar: inner width 6.6 units, 8.4 units from the floor to the dashed line. The largest whale is about 49% of the jar width.

Balance tables stay in `scripts/lib/defs.lua`; the pure combo rule is `scripts/app/scoring.lua`.
Shop products are authored only in `config/shop.json`, and pause width in `config/layout.json`.
See [EDITING.md](../games/DongleZoo/EDITING.md) for common edits and generation steps.

### Helper items

| Item | Effect | Coin price |
|---|---|---|
| Hammer | Removes the animal you tap | 120 |
| Shake | Shakes the jar so the animals settle again | 80 |
| Magic wand | Grows the animal you tap by one stage | 150 |

Two of each to begin with. When they run out, buy more with coins or get one for a rewarded ad.

## 5. Meta game (reasons to come back)

| Feature | Details |
|---|---|
| Coins | Score ÷ 30 at the end of a game. +20 the first time a new animal is made |
| Animal collection | Fills in as animals are met. Unmet animals are silhouettes |
| Daily gift | Seven consecutive days: 50 → 60 → 80 → 100 → 120 → 150 → 300 coins. Missing a day goes back to day 1. Doubled by watching an ad |
| Today's missions | Three of eight, chosen by the date (the same for every player). 40–90 coins each |
| Backgrounds | Sunny meadow (default), sunset beach 800, starry night 1,500 coins |
| Best score | Shown on the home screen and the HUD; "New record!" when beaten |

Coin economy (three games a day without ads, 1,500 points on average): 50 per game + about 120 from the daily gift +
about 170 from missions ≈ 440 coins a day. The sunset beach opens after about 2 days and the starry night around day
4. Buying items delays that, which is the reason to watch ads.

## 6. Monetization

| Means | Where | Principle |
|---|---|---|
| Rewarded ads | Continue (once per game, clears the upper half of the jar), double coins, one free item, double daily gift | Always the player's choice. The centre of the revenue |
| Interstitial ads | After a game ends, between "Play again" and "Home" | None in the first two games of a session, at least 150 seconds apart, never during play |
| Remove ads (in-app) | Shop, $2.99 | Removes interstitials; rewarded rewards are given at once without an ad |
| Coin packs (in-app) | Shop, $0.99 / $3.99 | 1,000 / 5,000 coins |

No banner ads (mis-taps and young players; the bottom of the screen is the item area).

### All-ages requirements (store policy)

- The game may fall under the Google Play Families policy, so ad SDKs are requested with child-directed settings
  (non-personalized, TFCD) and only Families-certified networks are used.
- No chat, external links, accounts or collection of personal data. Saves are local to the device.
- In-app purchases rely on the store's purchase authentication (parental confirmation).

### Implementation status — important

The game calls ads and purchases only through `scripts/lib/ads.lua`. That module asks the application embedding the
player through the engine's **host bridge** (`host.call` / `host.poll`,
[GAMEPLAY.md](GAMEPLAY.md#host-application-ads-purchases)).

- **Web**: `games/DongleZoo/web/host.js` connects to Google H5 Games Ads (the AdSense Ad Placement API). Real ads
  appear once the publisher id is put into `AD_CLIENT`; when it is empty the demo runs. Not verified with a real account.
- **Android**: the player is a NativeActivity without Java, so there is no layer yet that could call an ad or billing
  SDK (`PlatformHostCall` returns false). The APK runs with the demo overlays today. Real monetization needs a Java
  activity + AdMob / Play Billing (the unchecked lines of the work log below).
- **Demo mode**: without a host the same flow runs with demo overlays (a progress bar instead of an ad, a confirm
  dialog instead of a purchase). No real ads or charges. To keep demo purchases from simply handing out coins in a
  release build, set `D.DEMO_STORE = false` in `defs.lua`.

## 7. Screens

- Home: logo, best score, Play, Shop / Collection / Missions / Gift / Settings. A red dot when something can be claimed.
- Play: coins, score, best, pause and the next animal at the top; the jar in the middle; the three items at the bottom.
- Dialogs: pause, game over, shop, collection, missions, daily gift, settings, out of items.
- Languages: Korean and English (the device language on first run, switchable in settings). Font: Jua (OFL).

## 8. Art and sound

- The 11 animals and the app icon: images generated with Codex (`tools/art_src/codex/`), fitted to the physics
  circles by `tools/import_art.py`.
- Backgrounds, jar, effects and UI icons: drawn in code by `tools/make_art.py` (including fallback animals, so the
  game is complete without the generated images).
- 14 sound effects and one music track (a 35-second loop): synthesized by `tools/make_audio.py`.

## 9. Release checklist

1. `D.DEMO_STORE = false`; in `web/host.js` set `AD_CLIENT` and `TEST_ADS = false`.
2. Check `android.package` and `versionCode` in `project.json`. Build with the release key:
   `oe package games/DongleZoo --android --aab --keystore ...`.
3. Store listing: target age, "contains ads", privacy policy (stating that nothing is collected), portrait screenshots.
4. On a physical device: touch, sound, returning from the background, saves.

## Project structure and editing

| File under scripts/ | Lines | Responsibility |
|---|---:|---|
| `app.lua` | 92 | Startup, onUpdate order, action routing |
| `app/gameplay.lua` | 302 | Drop/merge/items/danger |
| `app/screens.lua` | 322 | Layout, home/HUD/dialogs, settings |
| `app/rewards.lua` | 139 | Finalization/gifts/store/ad actions |
| `app/effects.lua` | 54 | Visual effects and lifetimes |
| `app/scoring.lua` | 8 | Pure combo multiplier/cap/rounding |

The sole App script owns run state and passes services explicitly. Stateless method
modules import no siblings/services; existing board/save/audio/i18n/UI/ads modules keep
their implementation and native-snapshot lifecycle. Product IDs/EN/KO names/description,
display price and coin rewards live only in config/shop.json. Python emits the Lua table
and scene rows together; four or more rows use the engine's existing UIScroll. A new
coin pack needs no hand-authored scene/action/layout change, but a new reward type or
real store SKU needs its own implementation/provisioning.

`python games/DongleZoo/tools/make_content.py --scene-only` overwrites
scripts/config/shop.lua and scenes/main.scene.json, preserving prefabs. Full generation
also overwrites piece/effect prefabs. Direct scene edits do not survive generation.
make_art overwrites procedural textures, import_art installs illustrated animals/icon
afterward, make_audio overwrites audio; none is needed for a label/product/scoring edit.
The edit map and remaining prefab/defs geometry coordination are in EDITING.md.
Required project helpers now reload transactionally through script.reload/automatic polling
without Stop/Play for stateless gameplay/data edits (LUA_USABILITY.md 6.10). Attached App
state and top-level service table identities survive; onStart is not rerun. Stateful module
locals reset, existing callbacks/captured functions may remain old, and cached labels/locale
need explicit refresh. See EDITING.md for the i18n refresh and stateful-module restart limits.

## 10. Implementation status (work log)

- [x] Core play: aim, drop, merge, combo, danger line, game over (Box2D, `scripts/lib/board.lua`)
- [x] Layout for every screen ratio (engine: `input.viewSize` added); 9:16 / 9:20 / 3:4 / 16:9 screenshots checked
- [x] Three items, coins, shop (3 backgrounds, 3 in-app products), collection, daily missions, daily gift, settings, Korean/English
- [x] Art (generated + code), sound effects and music, saves (`save.*`)
- [x] Ads/purchases abstraction + demo provider, engine host bridge (`host.call/poll/language`,
      `host.state/configure/reply`), web adapter `web/host.js`
- [x] Automated tests `DongleZooGame`, `HostBridgeAndViewSize` (Windows `oe_tests`)
- [x] Web runtime rebuilt, `oe package games/DongleZoo --web`. In headless Edge (WebGL2): loading, start, drop,
      merge, save and host message delivery checked
- [x] Android runtime rebuilt (NDK r28c, arm64-v8a + x86_64); `oe package games/DongleZoo --android` APK and `--aab`
      produced. `apksigner verify` (v2), `aapt2 dump badging` (com.ainorix.donglezoo, portrait) and
      `zipalign -c -P 16` pass. Signed with the debug key
- [ ] Real ads in a browser with an AdSense account: impressions and the reward callback (needs a publisher id).
- [ ] Run on an Android device or emulator: this environment has neither, so the APK has never been run. After
      `oe package games/DongleZoo --android --install` check touch aiming, sound, returning from the background,
      saves and several screen ratios.
- [ ] Real ads and purchases on Android: implement `PlatformHostCall` / `PlatformHostPoll` in a Java activity (or
      through JNI) + AdMob, Play Billing.
- [ ] Play testing on a device and balancing (drop weights, coin prices); a full game played by hand.
- [ ] iOS (engine platform track, [APPLE.md](APPLE.md)).

## Lua UI pilot (M1b, 2026-10-06)

scripts/lib/ui.lua now delegates named acquisition, partial field updates, text and
visibility to the embedded oe.ui module. Raw numeric ID/kind caches were removed:
fresh acquisition prevents a name cache from targeting a reused ID after replacement.
Project localization (t: tags), runtime label overwrite, RGB/number formatting, toast
state and approximate textWidth remain unchanged. No app.lua split or game rule change
is included. Visibility changes only the widget; native ancestor hiding handles the subtree.

Menu/play/pause software hashes match their pre-migration captures exactly; game tests
and the complete 223-test desktop suite pass. Resume label, generated pause width/title
and combo multiplier edits were exercised then restored. Counts and screenshot evidence:
LUA_USABILITY.md 6.3. Web and both Android runtime ABIs were rebuilt with the new official
digest; M1b browser/device execution remains unverified. No commit was created.

## Coordinate pilot (M2a, 2026-10-06)

App:pointer converts normalized mouse input to logical pixels and calls explicit-camera
camera.screenToPlane on the existing z=0 play plane. App:toUI composes worldToScreen with
screenToCanvas, retaining the top-anchor centered X offset. App:layout still owns the
project's jar/camera/canvas fitting policy; score, save, localization, textWidth and app.lua
structure are unchanged. Menu/play/pause/drop hashes match the preserved legacy executable
and formulas; maximum sampled pointer coordinate difference is about 4.4e-8 world units.
Evidence and runtime/test results are in LUA_USABILITY.md 6.4 and its M2a work log.

## Text measurement pilot (M2b, 2026-10-06)

UI.textWidth delegates to oe.ui.measureText with the toast label's actual font/spacing/
bold/rich style and existing size 30; the character-category estimate is removed. Existing
pill maximum width 660 and padding 72, life/fade/localization/scoring/save behavior remain.
English/Korean pills now track Jua's real glyph advances; Japanese still uses missing-glyph
boxes because the bundled game font/default fallback lacks those glyphs. No font coverage
is added. Six language capture comparisons, actual popup edit task and test/runtime results
are recorded in LUA_USABILITY.md 6.5/8. app.lua is unchanged by M2b.


### M4 readability pilot (2026-10-07)

- [x] App split, explicit service/owner contract, pure scoring rule and EDITING.md.
- [x] Single-source shop JSON + generated Lua/scene; measured fourth coin pack purchase.
- [x] All 13 preservation checkpoints have identical PNG/frame hashes, run/save/tag state
  and host call order; fixed date 2031-03-03. Native old-save/native-restore/reload test passes.
- [x] Actual Resume EN/KO, product, combo-factor and popup width/title edits verified and
  reverted; player speed N/A. Counts and prior comparison: LUA_USABILITY.md 6.7.
- [x] Native build and 231 tests exit 0; 15 Lua checks errors/warnings 0; desktop package
  Play/quit and actual web menu→Play smoke pass. Runtime rebuild unnecessary (project only).
- [x] Required-helper live reload limitation superseded by the engine work in 6.10;
  stateless scoring/data edits reload live, with stateful/closure limits documented.
  Android device/real storefront/full packaged replay were not checked in this unit.

No engine/official module/template or save-schema changes. Generation's original catalog
produces identical semantic scene; final scene/prefab contents are preserved. Initial
generator CLI/test-setup corrections and complete execution evidence are recorded in
LUA_USABILITY.md 6.7/8 and ignored build/m4 artifacts. No commit.

### Required-only module reload verification (2026-10-07)

- [x] Persistent oe script session: scoring factor edit/reload/restore without Stop/Play;
  13 -> 15 direct points and 125 -> 150 actual merge gain. Same services/Scoring table,
  frame, prior score, probe field, coins and board count; script.errors empty; bytes restored.
- [x] Replay rerun: all 13 M4 checkpoint PNG/frame hashes and complete JSON state/save/host/
  localization-tag/scene summary match the recorded after-replay baseline exactly.
- [x] Native build/full 235 tests exit 0, including DongleZooGame and network/snapshot/render
  regressions. No game scripts, save schema, generators or scene files permanently changed.
- Runtime execution evidence and remaining device limitations: LUA_USABILITY.md 6.10/8.
