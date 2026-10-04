# Motion review after the first atlas integration

Grok follow-up: seven reviewed revisions are applied; the six rejected assets now
use their original single sprites at the user's request. See `GROK_MOTION.md`. The findings
below describe the rejected GPT candidates, not the newly applied Grok sheets.

The first pass verified texture/frame loading and inspected contact sheets, but
did not establish acceptable temporal motion. Runtime tests passing was not a
motion-quality verdict. The initial human report correctly rejects that output.

## Findings

- Keeper movement: four poses do not encode a clean alternating grounded walk.
  Ada/Suri include lifted kick-like poses; Bram's open-stride frames change body
  proportions. Loop wrap and rest/move switching visibly jump.
- Creature movement: the generic prompt mixed walking, floating and undulation.
  Leech changes silhouette area by 19.48%, Wisp 16.43%, Shade 14.93%, Stalker
  13.43%. These figures measure alpha-mask instability, not foot contact; animation
  and anatomy must still be judged visually.
- Idle: blinking/expression changes every 0.25 seconds create a one-second facial
  loop. Subtle breathing was replaced by changing expressions.
- Extraction: non-pixel extraction fits each connected pose independently. Larger
  leg/prop extents can therefore reduce apparent head/torso size. Equal cells and
  clean alpha do not imply a stable anatomical scale.
- Campfire: flame changes are expected, but bowl scale/location must remain fixed
  across lit frames and when ignition switches from unlit.
- All clips used the same movement recipe and rate despite different locomotion.

## Correction status

- [x] Measure all original atlas frames; save diagnostics to
  `build/wickbound-motion-audit.json`.
- [x] Build native-timing original/revised playback comparison with pause/step and
  idle/move controls (`build/wickbound-motion-review.html`).
- [x] Generate Ada prototype with explicit eight-phase grounded stepping, fixed
  head/torso and open-eye idle; extract with canonical sprite-gen tools.
- [x] Generate species-specific candidates for all 13 assets. Regenerate Bram's
  touching row, Eclipse's redesigned idle and Leech's almost-static move.
- [x] Compare all movement/lit contact sheets and native-timing playback in the
  visible browser; inspect scale, gait progression and loop boundaries.
- [x] Preserve installed assets. No complete replacement run was accepted.
  Require explicit per-state acceptance for both old and new installer inputs.
- [x] Adapt frame assertions to manifest clips rather than hard-coded indices.
- [ ] Produce a complete replacement set with convincing alternating support,
  stable anatomical scale across idle/move, and acceptable loop transitions.

## Candidate verdicts

These are rejection/hold findings, not claims that lower area variation proves a
motion pass. Idle candidates are held until cross-state identity and scale agree.
The installed first pass remains present and remains rejected by the user.

| Asset | Why the revised run is not approved |
| --- | --- |
| Ada | Repeated raised forward leg; missing convincing opposite-leg support phase. |
| Bram | Extraction gutters fixed, but raised forward leg still repeats and proportions shift. |
| Suri | Repeated forward-leg kick; body/accessory extents change apparent scale. |
| Brute | Frame 6 rises, frame 7 collapses; quadruped support progression is not coherent. |
| Stalker | More stable silhouette, but repeated lifted-paw poses do not establish a continuous quadruped gait. |
| Leech | Reduced outline variation, but ripple progression is too weak and antenna/head registration still varies. |
| Gloom | Outline stability improves, but eyes/face and skirt morph instead of a clear traveling crawl. |
| Skitter | Tiny feet change positions, but support alternation and idle/move registration remain unproven. |
| Shade | Tip extent changes cause substantial body/eye scale jumps; area range rises to 25.44%. |
| Wisp | Frame 6 sideways tip changes the whole core scale; area range remains 18.13%. |
| Eclipse | Original moon identity restored after idle regeneration; flame extent still changes sphere scale in move. |
| King | Foot poses and cloak proportions change without a convincing grounded shuffle. |
| Brazier | Flame changes still rescale/reposition the bowl; ignition alignment needs a fixed bowl anchor. |

## What needs to change next

Repeated GPT rows did not resolve all locomotion failures. Another whole-set
generation with the same recipe is not evidence of progress. Next candidates must
lock anatomical landmarks and preserve a common row scale during extraction;
alignment curation can correct registration, but cannot invent missing support
poses. Humanoid walking remains experimental until a full loop actually passes.
No frames were silently dropped, reordered, redrawn or retimed to conceal failure.

## Reproduction

Use the sprite-gen virtualenv Python for `tools/audit_motion.py` and
`tools/review_motion.py`. The latter writes separate candidates under
`build/wickbound-sprites-v2`, preserving first-pass sources. `--process` extracts,
composes, previews and inspects existing generated candidates. `--only keeper_ada`
limits a run. Generation remains on the previously chosen GPT subscription route.

The review page uses manifest rectangles and native frame timing; it does not
alter animation art, order or timing. A diagnostic range alone never grants a
motion pass. The browser must be visible for native animation timers to advance.
