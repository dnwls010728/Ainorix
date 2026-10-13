# Lighting and shadows

`PointLight` lights every direction from an entity's position. R2 adds `SpotLight`, a cone
aimed along the entity's local -Z axis, with color, intensity, range and a soft angular edge.
Both renderers use the same attenuation. R4 adds optional point and spot light shadows.

R3 adds cascaded directional shadows: the camera's view is split by depth so a large outdoor scene has
sharp nearby shadows, with stable texel snapping and blended transitions between maps.

## Point and spot shadows

Set `PointLight.shadows` or `SpotLight.shadows` to `true` (default **false**). The Inspector exposes a Shadows
checkbox on each light; the same reflected field is available through commands, scene files and Lua component
access. Existing scenes keep their previous lighting and software frame hashes. Shadows affect that light's
direct contribution only, leaving ambient, environment, other lights and emissive surfaces unchanged.

```bash
oe exec <project> component.set '{"id":"Lamp","type":"PointLight","values":{"shadows":true}}' --save
oe exec <project> component.set '{"id":"Spot","type":"SpotLight","values":{"shadows":true}}' --save
```

### Selection and projection

- At most **two local shadow lights per view**, shared between points and spots, independent of the sun.
  Candidates have positive radiance and a finite position/range, with their conservative world-axis range
  box intersecting the camera frustum. They are sorted by squared distance from source to eye, then entity id;
  point precedes spot if one entity has both. Disabled/zero-intensity lights do not consume the budget.
  Selection is recomputed per view/frame, including editor views and reflection-probe capture views.
- Only render-visible light slots compete: the first 16 points and first eight spots in entity-id order.
  The software renderer retains its earlier unlimited unshadowed point-light behavior. Shadow-enabled lights
  outside the budget remain lit without shadows; a conservative spot range box may qualify even when its cone
  points away. There is no hysteresis when the nearest lights change.
- A point uses **six world-axis perspective faces**, +X/-X/+Y/-Y/+Z/-Z, at 90 degrees. A spot uses one
  perspective map at twice its normalized outer half-angle, along the same rotation-only direction as lighting.
  Far plane is the light range; near plane is `max(0.001,min(0.1,range*0.01))`. The projection keeps far at least
  1.01 times near for degenerate direct callers. Point orientation is unaffected by entity rotation.
- Face selection uses the largest absolute ray component, ties X then Y then Z. Normal offset is 1.5 world
  texels at the receiver's perspective view depth. Point face selection is repeated after the normal offset,
  retaining receivers that cross a cube-face seam. Depth bias is half a world texel projected to normalized
  perspective depth. The same matrices, face choice and bias are used by software and GPU.
- Both renderers keep 3x3 PCF and opaque/cutout/displaced/skinned casters. Blended transparent or unlit meshes
  do not cast shadows. `MeshRenderer.castShadows` and sprite caster flags still apply. There is no per-light
  strength control: a fully occluded selected local light contributes zero direct light.

### Local-shadow budgets and limits

- **512x512 texels per map**, at most **12 maps** (two points). GPU uses a lazy **2048x2048 depth atlas**,
  four tiles per row, in one extra texture slot (binding 12). PCF taps clamp to each tile's texel centers;
  point PCF does not filter across cube faces, so small face-edge differences can remain.
- Software depth storage is up to **12 MiB**. A 32-bit GPU atlas is **16 MiB**, in addition to the sun/cascade
  maps. A point can draw each caster six times; two points can draw it twelve times. The GPU batches the union
  of local-map casters and clips geometry per tile. Maps are rebuilt every frame; no static shadow cache.
- Matrices and selection/bias data extend the existing light-data texture to **8x18 RGBA32F (2,304 bytes)**.
  Slots 72..73 describe selected lights, 74..81 map spot slots, 82..129 hold twelve matrices, 130..141 hold
  projection/bias parameters. Point shadow slots use the previously unused `point_color.w` uniform lane.
  The fragment shader has **203 vec4 uniform vectors and 13 sampler uniforms** (216 with a vector per sampler,
  below the GLES3/WebGL2 minimum of 224), using **13 of 16** texture units. No additional vertex-stage resources.
- Wide cones and distant receivers have larger world texels and coarser shadows. Geometry closer than the
  near plane is omitted; large world coordinates and very large ranges still limit floating-point precision.
  No transparent transmission, contact-hardening filter, or cached/temporal shadow updates.

### Local-shadow verification and example

The local example is `build/LocalShadows`: a warm shadowed point, a cool shadowed spot, a box and a sphere.

```bash
build/bin/oe editor build/LocalShadows
build/bin/oe render build/LocalShadows --out build/r4-software.png
build/bin/oe render build/LocalShadows --renderer gpu --out build/r4-gpu.png
build/bin/oe editor build/LocalShadows --screenshot build/r4-editor.png --select "Cool Spot"
```

`LocalShadowSelectionAndProjection` checks command fields/undo/serialization, opt-in defaults, offscreen and
inactive exclusion, nearest/id ordering, camera-dependent selection, all six projections and normal-offset
face-seam selection. `LocalShadowRendering` verifies point/spot occlusion, caster toggles, renderer parity,
and two point lights using twelve tiles with reversed selection/pool ordering. `PointShadowSixFaces` checks
actual receiver shadows and GPU parity for all six cube directions.

Point/spot mean RGB differences are **0.320/0.288 per channel out of 255**, and two points **0.261/255**.
All six faces produce more than 2,500 shadowed receiver pixels in the test scene.
Windows Release: **351 tests, zero failed checks**. Headless Edge on SwiftShader rendered the packaged
point/spot example without shader/WebGL warnings. Both Android ABIs build with 16 KB ELF load alignment;
not run on a device. The 640x360 example's mean
RGB difference is **0.084/255** (maximum 124 at raster/shadow edges). The Showcase software frame hash remains
`72ab07fb213fd42b`.

## Cascaded directional shadows

Set `DirectionalLight.shadowCascades` to **2, 3 or 4**, and `shadowDistance` to the far reach in world meters.
The Inspector exposes both fields on an existing directional light. The default count is **1**, retaining
the previous whole-scene or view-distance map, its normal/depth bias and frame hashes. Old scenes load unchanged.
Only the first directional light casts shadows, as before; `shadows` and `shadowStrength` still apply.

```bash
oe exec <project> component.set '{"id":"Sun","type":"DirectionalLight","values":{"shadowCascades":4,"shadowDistance":160}}' --save
```

- Counts normalize to 1..4 at rendering time. With multiple maps, positive distance is bounded by the camera
  far plane and 100,000 meters, with at least 0.01 meters beyond its near plane. Zero distance uses the camera
  far plane, capped at 1,000 meters. Invalid camera projections fall back to the legacy map.
- Perspective split depths are the average of uniform and logarithmic splits (fixed weight 0.5); orthographic
  views use uniform splits. Selection uses linear depth along camera forward, not radial distance.
- The last **10%** of each depth interval blends linearly to the next map. Its receiver slice starts at the
  previous map's blend start, so both maps cover the overlap. The last map fades to fully lit over its last
  10%, and nothing is shadowed beyond the far reach. There is no hard cut between maps.
- Each receiver slice is bounded by a sphere, its radius rounded up to 1/16 meter with a two-texel guard on
  each side. Camera rotation keeps the same spherical extent; the light's axes are world-anchored. The center
  snaps to whole shadow texels, so sub-texel camera motion does not move the sampling grid fractionally.
- The light-space depth range includes the complete caster/receiver scene box plus a one-meter guard,
  retaining casters between the light and the slice even when outside the camera view. Normal bias is 1.5
  world texels; cascade depth bias is half a world texel divided by the map's light-space depth span. This
  avoids losing small nearby shadows to the single map's fixed depth bias in a very large level.
- Both renderers use the same splits, matrices, bias, selection and blend weights (`FitShadowCascades`,
  `ShadowCascadeIndex`, `ShadowCascadeBlend`). Maps retain 3x3 PCF and existing opaque/cutout/displaced/skinned
  caster handling. GPU hardware comparison filtering and MSAA differ slightly from software rasterization.

### Cascade budgets and limits

- **Four maps maximum; 1024x1024 texels per map** in both renderers. GPU uses a lazy **2048x2048 depth atlas**
  in the existing shadow texture slot: top-left, top-right, bottom-left, bottom-right. PCF taps clamp to their
  tile's texel centers to prevent leakage from neighbors. Two or three cascades leave other tiles unused.
  Single-map resolution remains unchanged (software 1024, default GPU 2048).
- Software depth storage is up to 16 MiB; a 32-bit GPU atlas is 16 MiB. Four cascades can draw a caster four
  times; the GPU batches the union of visible-to-any-cascade casters, leaving per-tile clipping to the GPU.
  More maps trade additional shadow work for better near-camera detail; there is no per-frame update cache.
- The mesh fragment program uses **203 vec4 uniform vectors plus thirteen sampler uniforms**, within the
  GLES3/WebGL2 minimum of 224 even allowing a vector per sampler, and **13 of 16** texture units. Vertex-stage
  resource use is unchanged; the pre-existing skin/vertex-graph path still needs minimum-capability device checks.
- Very large/far slices have wider world texels and softer/coarser shadows. Scene bounds stretched far along
  the light direction can still reduce depth precision. No temporal filtering or per-cascade caster partitioning.

### Cascade verification and example

The local example is `build/Cascades`: pairs of pillars from nearby to 110 meters away, a large floor,
four shadow maps and a 160-meter reach.

```bash
build/bin/oe editor build/Cascades
build/bin/oe render build/Cascades --out build/r3-software.png
build/bin/oe render build/Cascades --renderer gpu --out build/r3-gpu.png
build/bin/oe editor build/Cascades --screenshot build/r3-editor.png --select Sun
```

`CascadedShadowFitsAndStability` checks full overlap coverage, split/boundary selection, blend continuity,
whole-texel movement, finer nearby resolution, orthographic splitting and unchanged legacy fits.
`CascadedShadowRendering` drives the component through commands, checks serialization and actual shadow
darkening, compares 2/3/4 maps against GPU and switches to/from the legacy texture in one renderer.
The 640x360 example has mean RGB difference **0.265/255** (maximum 114 at raster/shadow edges) against D3D11.
Windows Release: **348 tests, zero failed checks**. The test scene has shadowed ground pixels in all four
cascades (176, 171, 25, 12); GPU mean differences for 2/3/4 maps are 0.577/0.569/0.562 per channel.
Headless Edge (WebGL2 on SwiftShader) renders the four-map packaged example without shader/WebGL warnings.
The existing Showcase software hash remains `72ab07fb213fd42b`.

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
| `shadows` | `false` | Enable shadows when selected within the shared two-local-light budget |

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
- R4 shadows are optional; without `shadows`, walls do not block a spot's direct light. See Point and spot shadows above.
- GPU light data is a nearest-sampled **8x18 RGBA32F texture (2,304 bytes)** at binding 11. Rows 0..2 hold
  position/range, direction/outer cosine and color/inner cosine for eight spots. The remaining texels hold the
  sky's nine irradiance coefficients and up to four probes' nine coefficients each, unchanged mathematically.
  The immutable image is replaced only when values change, including between editor views.
- After R4 the mesh fragment shader uses **13 of 16** texture units and **203 vec4 uniform slots**, plus thirteen sampler
  uniforms (even allowing one vector per sampler, 216 is below the GLES3/WebGL2 minimum of 224). No new vertex
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

R2 validation: Windows Release **346 tests, zero failed checks**. Headless Edge on SwiftShader rendered the
spotlight demo and the existing probe room without shader/WebGL warnings; the probe capture took 185 ms.
Android arm64-v8a and x86_64 players build successfully with 16 KB ELF load alignment; no device run.

## Status

R2 spotlights and R3 cascaded directional shadows are complete: both renderers, reflected editor fields/guides,
API documentation, 348 Windows tests, and rebuilt web/Android runtimes. Headless Edge checked the packaged
four-cascade example; earlier R2 checks covered spotlights and the reflection-probe room. R4 local-light
shadows are delivered by the work log below.

- [ ] Android physical-device rendering (build only here; package/install and verify on GLES3 hardware).
- [ ] Linux/EGL rendering (requires a Linux environment).
- [ ] Existing skin/vertex-graph shader execution on minimum-capability devices; R2/R3 leave that vertex budget unchanged.

## Implementation status (work log)

- [x] R4 M1: PointLight/SpotLight shadow flags, shared deterministic in-view/nearest light selection and
  perspective maps; software/GPU local-shadow atlas, filtering and bias; behavior/parity/regression tests,
  API/docs, example and editor screenshots.
- [ ] R4 M2: rebuild web and both Android players, verify packaged local shadows in headless Edge,
  refresh runtime hashes and complete the roadmap.
- [ ] Android physical-device and Linux/EGL local-shadow rendering (unavailable here).
