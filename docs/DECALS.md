# Box-projected decals

`Decal` projects a texture and tint onto opaque/cutout surfaces inside its oriented box. It changes
surface base color before lighting, without adding geometry or changing depth/picking. Both renderers
share bounded selection, atlas sampling, draw order and box/normal fades.

```json
{"name":"Floor stain","components":{"Transform":{"position":[0,0.03,0]},
 "Decal":{"texture":"assets/textures/stain.png","size":[2,0.3,2],
          "color":[0.7,0.05,0.02],"opacity":0.8,"edgeFade":0.1,"order":10}}}
```

Pass this object to `entity.create`, or add/edit `Decal` through `component.add/set`. The same
schema is available to CLI, HTTP, MCP, Lua component access and the editor Inspector. **Create >
Rendering > Decal** creates a solid colored projector. Its hierarchy icon and Scene marker identify
the entity; selecting it draws the oriented box and projection direction. Creation labels include
Korean and Japanese translations. It needs no `MeshRenderer`.

## Controls and projection

- `texture`: project-relative image with alpha; empty means solid color. Missing assets are reported
  by the asset manager and show magenta. Textures follow the normal asset hot-reload path.
- `size`: full local box dimensions, default `[1,1,1]`, positive and at most 1000 on each axis.
  Position/rotation/scale and all parents apply. Local X/Z are image axes, local Y is projection depth.
  Image top points toward local +Z; rotate the entity to turn the image or project onto a wall.
- `color`: RGB texture tint, default white, bounded 0..1.
- `opacity`: 0..1, default 1, multiplied by texture alpha. Zero skips selection/atlas sampling.
  A Lua script can animate this field on simulated time for a timed fade; there is no wall-clock lifetime.
- `edgeFade`: 0..0.5, default 0.1. Coverage fades over this fraction of each normalized box axis
  near all six boundaries. Zero gives hard box edges. A thin box must overlap the receiving surface,
  rather than place it at a feathered boundary.
- `normalThreshold`: -1..0.99, default 0.2. Rejects geometric surface normals whose dot with local
  +Y (transformed by inverse-transpose) is below the threshold. Above it coverage fades linearly to
  full at dot 1. This avoids painting perpendicular/backward surfaces inside the same box. Normal
  maps and graph lighting-normal overrides do not change the projection test.
- `order`: integer, default 0. Lower paints first, higher covers it; entity id breaks equal-order ties.
- `enabled`: default true. Disabled boxes do not load textures or contribute samples.

The box is tested per fragment in local space, including the actual skinned/displaced world position.
It paints every eligible surface in its volume; there is no occlusion ray or target-entity mask, so a
deep box can mark several parallel surfaces. Keep projection depth shallow for bullet holes.
Invalid/nonpositive size, nonfinite/zero opacity, nonfinite or near-singular transforms are skipped.
Oversized dimensions/opacity are bounded; nonfinite color/fade/threshold use white/0.1/0.2 defaults.

## Rendering and fixed limits

At most **16 visible enabled boxes per view**, selected by order then entity id. Higher-order boxes
beyond that budget are skipped, so remove old marks or manage enabled/opacity when spawning many.
Textures are loaded only for the selected boxes. A shared cached **1024x1024 RGBA8 atlas** has sixteen
256-square tiles, each with a one-pixel duplicated border and **254x254 useful image pixels**. Both
renderers use the same resampled pixels and bilinear clamp sampling. Images of any size/aspect are
resampled to a square; set box X/Z dimensions to get the desired world aspect. The atlas is rebuilt
only when selected texture slots or versions change, not when tint or opacity changes.

Decals blend into base RGB after a surface graph's color and alpha-cutout evaluation, before ordinary
PBR or unlit rendering. Receiver alpha, lighting normals, metallic/roughness, emission, shadows, depth,
entity IDs and collision geometry are unchanged. Thus decals receive existing lighting and shadows,
cutout holes stay holes, transparent/additive receivers are excluded, and UI/background/selection are
unaffected. Camera HDR/fog/SSAO/DOF/bloom effects still run normally. A fully reflective planar surface
can cover its own base-color decal, while its live capture includes decals on reflected scenery.
Probe captures bake eligible marks into their static pictures; rebake to refresh animated marks.

GPU binding 15 is the atlas, reusing the planar clamp sampler. The data texture expands to **8x33
RGBA32F**, with seven texels per decal. SSAO lens settings move from one uniform vector to texel 148.
The mesh fragment program now uses **208 vec4 uniform vectors plus 16 sampler uniforms** (224 with a
conservative vector per sampler, the WebGL2/GLES3 minimum), and **16 of 16 texture units**. There are
no added vertex-stage resources or render targets. Further mesh effects must reuse resources or split
passes. Empty scenes skip decal atlas work and keep prior software hashes.

This version supports color/alpha decals, not normal/roughness/emissive projection, texture animation,
screen-space normal reconstruction or transparent receivers. Cost grows with visible boxes and shaded
fragments. `build/Decals` demonstrates floor stains, a marking and rotated wall impacts:
`build/bin/oe editor build/Decals`. Select a decal to see its box; toggle enabled or animate opacity.

## Implementation status (work log)

- [x] Add the component, shared projection/atlas cache and both renderer paths within GLES3 minimums.
- [x] Add editor creation/box guides and verify projection, order, fade, exclusions and renderer parity.
- [x] Document limits, regenerate API reference and leave a demonstration under `build/Decals`.
- [ ] Rebuild web/Android players and verify the WebGL2 demonstration.
