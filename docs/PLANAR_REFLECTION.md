# Planar reflections

`PlanarReflection` on a flat `MeshRenderer` makes a mirror or water surface show the current scene,
including moving entities, from a camera reflected across its plane. Both renderers share the
camera and plane math. One visible surface is selected per view; captures do not recurse.

```json
{"name":"Water","components":{"Transform":{"scale":[8,1,8]},
 "MeshRenderer":{"mesh":"plane","color":[0.1,0.2,0.3],"castShadows":false},
 "PlanarReflection":{"strength":0.7,"resolution":512}}}
```

Use `entity.create` with that object, or add `PlanarReflection` to an existing flat mesh through
`component.add/set`. CLI, HTTP, MCP and Lua component access share the same reflected schema. In the
editor use **Create > Rendering > Planar Reflection**. The Inspector edits the controls; selecting the
surface draws a plane marker and normal. Korean and Japanese creation labels are included.

## Plane and controls

- `normal`: local-space plane normal, default `[0,1,0]` for the built-in `plane`. It is normalized
  after the entity's world inverse-transpose, including hierarchy, rotation and nonuniform scale.
- `offset`: local-space plane origin, default `[0,0,0]`; the full world transform applies.
- `strength`: 0..1, default 1. Blends the ordinary lit/unlit material RGB toward the reflection.
  Zero skips capture. Emission is added afterwards, then ordinary fog/post effects and surface opacity.
- `resolution`: square capture size, 64..1024, default 256. Any integer in that interval is supported.
- `clipBias`: 0.001..0.1 world meters, default 0.01. Geometry behind the plane and a thin band above
  it is clipped, preventing self-reflection leaks at contact. Reduce it for small/thin objects.
- `enabled`: default true; false restores the ordinary material without capture.

Runtime values are bounded even when loaded from old/hand-edited files. Nonfinite strength or plane
geometry, zero normals, degenerate clipping, and a camera on/behind the plane disable selection.
Nonfinite bias uses 0.01. A `MeshRenderer` is required; the user must make its geometry coincide with
the component's flat plane. Curved, skinned or vertex-displaced receivers are not supported. Moving,
skinned, displaced, textured, masked and transparent objects in the reflected scene keep their normal
rendering behavior. Only the receiving entity is excluded; its other child entities remain visible.

## Rendering contract and limits

At most **one visible front-facing receiver per view** is selected, by squared distance to its plane
origin and then entity id. Nonselected surfaces use their ordinary material. The mirror renders the
current scene again every frame, including dynamic bodies, animation and VFX, without advancing the
simulation. It reflects the eye and view matrix, flips projection X to preserve front-face culling,
and uses a shared oblique near plane for precise clipping in perspective and orthographic cameras.
Screen projection and bilinear clamp sampling match between renderers. No GPU readback is used for
the reflection. Main-view depth and picking still refer to the surface, not reflected entities.

Captures use RGBA8 scene color before camera post effects, and may clip bright HDR emission. The
receiving view applies its effects once after reflection blending. Captures retain ordinary lights,
shadows, sky and reflection probes, but exclude UI, grid, selection/debug overlays and further planar
captures. Probe baking does not include planar reflections. Other mirrors in the capture show their
ordinary material, preventing recursion. This is a sharp color reflection: there is no roughness blur,
Fresnel, refraction, water distortion, temporal cache, reflected picking or multiple-mirror recursion.
It costs an additional full scene render; use a lower resolution or disable it on slower devices.

GPU reflection targets follow the normal RGBA8/MSAA path. The light-data texture expands to **8x19
RGBA32F** to carry the reflected view-projection matrix; the reflection image uses binding 14.
There are **209 fragment vec4 uniform vectors plus 15 sampler uniforms** (224 with a conservative
vector per sampler, the GLES3/WebGL2 minimum) and **15 of 16** texture units. No new vertex-stage
resources. Additional future features must move data out of uniforms or reuse bindings. The existing
skin/vertex-graph minimum-device budget remains unverified. No float render-target extension is
required specifically for planar captures; HDR/SSAO/DOF in the receiving view retain their requirements.

Scenes without an enabled visible component keep their previous software hashes. `build/PlanarReflection`
demonstrates a moving character over a reflective floor and a green object below the plane that should
not appear in its reflection. Open `build/bin/oe editor build/PlanarReflection` and press Play. Turn
`enabled` off on Reflecting floor to compare, or reduce `strength` for a water-like blend.

## Implementation status (work log)

- [x] Add reflected-camera selection, component controls and both renderer paths with plane clipping.
- [x] Add editor creation/guides and verify motion, clipping, selection, determinism and renderer parity.
- [x] Document limits, regenerate API reference and leave a demonstration under `build/PlanarReflection`.
- [ ] Rebuild web/Android players and verify the WebGL2 demonstration.
