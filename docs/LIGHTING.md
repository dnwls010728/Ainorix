# Local lights

`PointLight` lights every direction from an entity's position. R2 adds `SpotLight`, a cone
aimed along the entity's local -Z axis, with color, intensity, range and a soft angular edge.
Both renderers use the same attenuation. Spot shadows are reserved for R4.

## Using a spotlight

Create > Rendering > Spot Light adds a downward-facing light in the editor. The Inspector edits
the reflected fields; selecting the light shows both cone boundaries and their radial range.
The same component works through `entity.create`, `component.set`, scene files and Lua component access.

```bash
oe exec <project> entity.create '{"name":"Spot","components":{"Transform":{"position":[0,4,0],"rotation":[-90,0,0]},"SpotLight":{"color":[1,0.8,0.5],"intensity":4,"range":8,"innerAngle":15,"outerAngle":30}}}' --save
oe exec <project> component.set '{"id":"Spot","type":"SpotLight","values":{"outerAngle":40}}' --save
```

| Field | Default | Rendering contract |
|---|---|---|
| `color` | `[1,0.85,0.6]` | RGB multiplied by intensity; negative channels render as zero |
| `intensity` | `1.5` | Brightness on the axis at the source; negative values render as zero |
| `range` | `6` | Radial distance in world meters; at least `0.01` |
| `innerAngle` | `20` | Cone half-angle in degrees; clamped to `0..outerAngle` |
| `outerAngle` | `30` | Cone half-angle in degrees; clamped to `0.1..89` |

Normalization affects rendering and the guide, leaving the stored values intact. Non-finite brightness/color
render as zero; non-finite range/angles use defaults. Angles are measured from the axis: an outer angle of 30
means a 60-degree-wide cone. Equal inner and outer angles give a hard edge; inner angles above outer clamp
to the outer angle. A zero inner angle is valid.

Position uses the complete world transform. Direction applies the entity's rotation and every ancestor's
rotation to local -Z, child first; scale, reflections and shear do not change direction or range. A parent
scale still affects the position of an offset child. The editor uses the same normalized light data;
the cone rim is at `range*cos(angle)` along the axis with radius `range*sin(angle)`.

## Shading and budgets

For distance `d`, attenuation is `(1-d/range)^2` inside the range. Let `c` be the cosine between the travel
direction and the ray from the light to the surface. Angular attenuation is `t*t*(3-2*t)` with
`t=clamp((c-cos(outerAngle))/(cos(innerAngle)-cos(outerAngle)),0,1)`; equal angles use a step at the boundary.
Their product multiplies the existing metallic-roughness BRDF in both renderers, including lit sprites
and shader materials. Unlit surfaces ignore all direct lights. Lights do not create a visible beam in air.

- At most **eight spots per frame**, the first eight entity ids, independent of point-light slots. Off or
  out-of-view spots still occupy a slot; there is no distance/view selection yet. Every view and probe capture
  uses this order. The software renderer retains its existing point-light behavior; the GPU still allows
  16 point lights and four directional lights.
- No spot shadows or occlusion: walls do not block these lights. R4 will add shadow selection using the source
  entity id, world position, direction, range and cone cosines already carried by `RenderSpotLight`.
- GPU light data is a nearest-sampled **8x9 RGBA32F texture (1,152 bytes)** at binding 11. Rows 0..2 hold
  position/range, direction/outer cosine and color/inner cosine for eight spots. The remaining texels hold the
  sky's nine irradiance coefficients and up to four probes' nine coefficients each, unchanged mathematically.
  The immutable image is replaced only when values change, including between editor views.
- The mesh fragment shader uses **12 of 16** texture units and **187 vec4 uniform slots**, plus twelve sampler
  uniforms (even allowing one vector per sampler, 199 is below the GLES3/WebGL2 minimum of 224). No new vertex
  attributes or vertex-stage uniforms. The existing skin/vertex-graph uniform budget is unchanged by R2;
  minimum-capability vertex-stage devices still need independent validation.

## Verification and demo

The local example is `build/Spotlight`: two warm/cool spots with spheres and a floor.

```bash
build/bin/oe editor build/Spotlight
build/bin/oe render build/Spotlight --out build/r2-software.png
build/bin/oe render build/Spotlight --renderer gpu --out build/r2-gpu.png
build/bin/oe editor build/Spotlight --screenshot build/r2-editor.png --select "Warm Spot"
```

`SpotLightContract` checks command edits/undo, serialization, parent rotation with mirrored/zero scale,
normalization, the soft/hard angular edge and deterministic light selection. `SpotLightRendering` checks
light direction, radial cutoff, disabled/unlit behavior, soft/hard/wide cone software/GPU parity and the
fragment resource budget. `NativeEditorLightGuides` includes a selected spot and verifies guides do not
mutate the scene or undo history. Existing sky/probe parity tests cover the shared irradiance data.

The Showcase software frame hash before and after R2 is `72ab07fb213fd42b`. The 640x360 example has a mean
RGB channel difference of approximately **0.031/255** between software and D3D11; the maximum (126) is at
rasterized edges, where GPU MSAA differs. Direct spotlight parity tests have mean differences below 0.05/255.

Windows Release validation: **346 tests, zero failed checks**. Headless Edge on SwiftShader rendered the
spotlight demo and the existing probe room without shader/WebGL warnings; the probe capture took 185 ms.
Android arm64-v8a and x86_64 players build successfully with 16 KB ELF load alignment; no device run.

## Implementation status (work log)

- [x] R2 M1: component/schema, shared light data, software/GPU shading, editor creation/icon/cone,
  API and feature documentation, automated tests and software/GPU/editor screenshots.
- [ ] R2 M2: rebuild web and Android players; package and check the web demo in headless Edge.
- [ ] Android physical-device rendering (not available here; build only).
- [ ] Linux/EGL rendering (requires a Linux environment).
