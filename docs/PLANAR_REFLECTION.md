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
shadows, sky (the `Skybox` background too, test `PlanarReflectionShowsSky`) and reflection probes, but exclude UI, grid, selection/debug overlays and further planar
captures. Probe baking does not include planar reflections. Other mirrors in the capture show their
ordinary material, preventing recursion. This is a sharp color reflection: there is no roughness blur,
Fresnel, refraction, water distortion, temporal cache, reflected picking or multiple-mirror recursion.
It costs an additional full scene render; use a lower resolution or disable it on slower devices.

GPU reflection targets follow the normal RGBA8/MSAA path. The light-data texture expands to **8x33
RGBA32F** to carry the reflected view-projection matrix; the reflection image uses binding 14.
With R7 decals there are **208 fragment vec4 uniform vectors plus 16 sampler uniforms** (224 with a conservative
vector per sampler, the GLES3/WebGL2 minimum) and **16 of 16** texture units. No new vertex-stage
resources. Additional future features must move data out of uniforms or reuse bindings. The existing
skin/vertex-graph minimum-device budget remains unverified. No float render-target extension is
required specifically for planar captures; HDR/SSAO/DOF in the receiving view retain their requirements.

Scenes without an enabled visible component keep their previous software hashes. `build/PlanarReflection`
demonstrates a moving character over a reflective floor and a green object below the plane that should
not appear in its reflection. Open `build/bin/oe editor build/PlanarReflection` and press Play. Turn
`enabled` off on Reflecting floor to compare, or reduce `strength` for a water-like blend.

## Status

Implemented in both renderers with editor creation/Inspector controls and a plane guide. 354 Windows tests
pass. PlanarReflectionSelectionAndBounds and PlanarReflectionMotionClippingAndParity verify selection,
invalid settings, hierarchy/nonuniform scale, recursion exclusion, moving bodies, plane-side clipping,
horizontal/vertical mirrors, perspective/orthographic views, serialization, thread-count determinism,
and unchanged depth/IDs/UI. The demo's mean software/D3D11 RGB difference is 0.373/255 at frame zero
and 0.388/255 after 90 simulation frames. Showcase retains hash 72ab07fb213fd42b without the component.
CLI PNGs and an editor screenshot were visually checked.

Web and both Android players were rebuilt. Headless Edge/SwiftShader compiled and linked the mesh
program and displayed the packaged moving-character reflection without WebGL errors. Both Android
ELF libraries retain 16KB LOAD alignment. One Android compiler frontend crash in unchanged VfxExpression.cpp
was resolved by retrying with reduced build parallelism; no source workaround was needed.

- [ ] Planar rendering on Android physical devices, Linux/EGL and real browser GPUs.
- [ ] Existing skin/vertex-graph execution on minimum-capability WebGL2/GLES3 devices.
