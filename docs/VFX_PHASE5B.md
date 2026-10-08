# VFX phase 5b: bounded typed expressions

Use Effects > Expressions to choose Birth/Update and a channel, type an expression and Apply Expression.
Each application is one document edit; invalid text stays in the input while the valid asset is preserved.
The same serialized text works through `vfx.set`, CLI, HTTP and MCP. Value nodes remain phase 5c;
all six acceptance effects must first pass the text-only 5b+ gate.

Open `samples/VFXLab/effects/spiral_expressions.vfx.json` or load `scenes/expressions.scene.json`.
The spiral ring rises, rotates and fades without a gameplay transform script. Its radius, height and
RGB tint are expression-only parameters (`targets:[]`). The core proof uses the existing soft glow
texture; it is not the six-effect acceptance gate or a new preset.

## Authoring contract

Only source text is serialized, under `ParticleEmitter.expressions`:

```json
{
  "birth": {"position": "vec3(index,0,0)", "custom0": "random"},
  "update": {
    "position": "vec3(cos(age*2*pi),age*param.height,sin(age*2*pi))",
    "scale": "vec3(1,1-pow(1-age,3),1)",
    "opacity": "curve(age,0,0,0.1,1,0.8,1,1,0)"
  }
}
```

- Position, velocity, rotation, scale and color require `vec3`; size, opacity and custom0..3 require
  scalars. Arithmetic broadcasts scalars to vectors; comparisons require scalars. Ordinary decimal
  literals (including `.5`), unary +/-, parentheses and + - * / < <= > >= == != are supported.
  Scientific literals, assignments, loops, strings, user functions and recursion are not supported.
- Functions: vec3, min, max, abs, clamp, lerp, select, sin, cos, pow, exp, sqrt, floor, fract, dot,
  length, curve. `select(condition,a,b)` evaluates both branches. `curve(t,t0,value0,...)` has 1..16
  ascending literal ages in 0..1, same-type values, linear interpolation and endpoint clamping.
- Inputs: normalized age, lifetime in seconds, emitterTime, systemTime, accepted index/batch count,
  stable random, speed, birthPosition/Velocity/Rotation/Scale/Color, birthSize, birthOpacity and pi.
  Vector inputs and vector parameters support `.x/.y/.z`; arbitrary expression postfix swizzles do not.
  `param.name` reads asset numeric scalar/vec3 parameters; expression names use identifier characters
  (ASCII letters, digits and underscores). Hyphenated legacy parameter names remain valid for field mappings.
  Standalone ParticleEmitter expressions have no asset parameters.
- All Birth channels read one frozen pre-expression context. Position/velocity replace shape/offset
  position and initial velocity in emitter coordinates. Rotation/scale replace captured XYZ birth
  values. Color/size/opacity replace start/end values. Custom channels replace captured custom values.
  Outputs are then recaptured for Update; Birth never runs again on live particles after a parameter edit.
- Update channels read one frozen context after age advances, before prescribed velocity/forces.
  Newborns also evaluate Update at age zero. Update position is an offset on separately integrated
  motion, rotation adds to captured XYZ rotation, scale multiplies captured scale, velocity replaces
  velocity before gravity/turbulence/drag. Color/size/opacity/custom replace the corresponding curve
  output. Missing expressions use existing curves and fields; legacy Z spin still applies.
- Position/velocity vectors use emitter coordinates. World particles transform them by their captured
  birth frame; local particles follow the owner. Speed is the magnitude of stored particle velocity,
  before this step's Update velocity/forces (local-space speed for local particles). Birth size excludes
  the captured sizeVariation multiplier; Birth and Update size each apply that multiplier exactly once.
  Scale/size clamp nonnegative; opacity clamps 0..1. RGB and custom data allow signed/HDR values within
  the finite instruction range. Standard material/color handling applies at rendering.
- emitterTime follows the existing emitter emission clock; stopped emission freezes that clock.
  systemTime is the parent VFXSystem clock sampled at step start, including during stopped emission
  while particles age. Standalone emitters use emitterTime. Newborns see the emission clock at birth.
  Pause freezes simulation clocks; clear does not reset them. Asset restart/new system resets them.
- Cached immutable programs are internal, shared by native snapshots; current parameters and clocks
  remain runtime values. Hot reload preserves captured birth values and replaces Update programs.
  Scene JSON stores source only, not instructions/caches/particle state.

`vfx.schema.expressions` publishes functions, inputs, limits and orbit, spiral, pulse, ease-out,
rise/hold/fall and random-range snippets. `vfx.expression_eval {source,type?,inputs?,params?}` is a
read-only diagnostic returning typed value and instruction count. `vfx.check` reports
expressionInstructions and expressionStepInstructions. Errors identify block, stage/channel and
line/column, with repair hints; rejected edits leave the valid draft and runtime untouched.

## Numeric and cost bounds

Each source has at most 1024 bytes, nesting 32 and 64 instructions; the sum is at most 256 per emitter
and 512 per asset. Capacity reserves `sum(maxParticles * 5 * (birthCost + updateCost)) <= 1000000`
instructions per step, conservatively covering initial Update evaluation and death-birth passes.
This uses configured capacities, even when the global particle budget is lower. Command preview/analysis
add a request bound of 100000000 reserved instructions across frames and passes; editor scrubbing uses
the same bound. Existing particle-step and capacity limits still apply. Costs count compiled operations,
not wall-clock time; all math functions have bounded internal work.

Every instruction clamps to +/-65504. Division by zero, negative square root and negative fractional
power return zero; NaN and subnormals become zero. `0^0` is 1, other zero-base powers are zero. Own
float polynomial/Newton math avoids platform transcendental libraries and uses the engine's disabled
floating-point contraction. exp below -20 returns zero and above 11.1 saturates. These are bounded
visual approximations, not scientific math. Tests check sin absolute error below 0.0005 over 0..2*pi,
pow below 0.00002 for bases 0.1..1.1/exponent 2.3, exp below 0.0001 for -4..4. Large angles lose normal
single-precision resolution. Keep phase inputs small with fract when appropriate.

## Verification

- Compiler/command tests cover scalar/vector grammar, parameters/components, curves, located errors,
  finite handling, type rejection and source/depth/channel/emitter/effect/request budgets.
- Channel tests cover frozen birth values, accepted index/count, stable random, expression precedence,
  non-accumulating offsets, world frames, size variation once, rendering appearance and native restore.
  Asset tests cover live parameter changes, rejected draft edits, undo/redo, atomic save and hot reload.
- Native window events cover text input, invalid-source refusal, individual Apply history, undo/redo
  and clearing. Empty-target/vector parameter UI and retaining expression parameters on wire removal
  are covered too. Field and graph views share that widget and document. English/Korean UI and software/
  D3D11 sample previews are inspected.
- Scalar math reference hash: `32d9e91c45973a93` (606 evaluated floats including sin/cos/pow/exp/sqrt/curve).
  Windows/Node both assert it. The 128px four-tile sample preview asserts `b35ab4ebab9114ff`; D3D11 preview
  is `6bea8137584bf718`. Times: 0.2/0.8/1.5/2.8s; 48 particles, 85 compiled instructions, 40800 reserved/step.
  Analysis reaches peak 48/96 and finishes at 3.03333333s.
- The measured 1000-particle workload uses 32 instructions/particle and 160000 reserved/step.
  Windows isolated run: 0.374 ms/step (120 steps); concurrent build/test load varies this measurement.
  Node/WASM: 0.271 ms/step for the same workload. Final suite results are recorded below. Timings are observations, not test thresholds.
- Legacy particle and 5a preview hashes remain asserted by the full regression suite.

## Implementation status (work log)

- [x] Typed compiler, located errors and bounded instruction programs.
- [x] Own bounded math, finite handling, curve sampling and named schema snippets.
- [x] Birth/update integration, parameters, cached programs, clocks and native restores.
- [x] Conservative emitter/effect/step/request budgets and headless diagnostics.
- [x] Shared-document text editing, event tests, proof asset/scene and preview inspection.
- [x] Windows Release: all 293 tests pass, zero failed checks and no new C++ warnings.
      Hot-reload fixture timestamps are advanced explicitly to avoid rapid-write timestamp collisions.
- [x] Node/WASM: all 239 tests pass, including identical math/preview reference hashes.
      The pre-existing unused ProcessFixtureDir warning remains.
- [x] Prebuilt Web and Android arm64-v8a/x86_64 players rebuilt; runtime README hashes refreshed.
- [ ] Android device, browser GPU and Linux/EGL execution; run the sample and rendering suite there.
- [ ] 5b+ text-only acceptance assets (ice pillar, tornado, shield, shockwave, rain/snow, healing aura).
