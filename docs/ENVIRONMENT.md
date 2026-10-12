# Environment & image-based lighting

A long feature (DESIGN.md §5). It gives a scene an environment — a sky that lights surfaces, that
smooth/metallic surfaces reflect and that is drawn behind the scene — so PBR materials stop looking
flat. Built in milestones; each milestone is a commit.

The environment is the `Skybox` component (one per scene; Create > Rendering > Sky in the editor),
plus any number of `ReflectionProbe` components (Create > Rendering > Reflection Probe) for places
that should reflect their own surroundings rather than the sky.
See [RENDERING.md](RENDERING.md) for how it feeds lighting, and `docs/API.md` for its fields.

```bash
# three colors
oe exec <project> entity.create '{"name":"Sky","components":{"Skybox":{"sky":[0.2,0.4,0.8],"horizon":[0.9,0.7,0.5],"ground":[0.2,0.2,0.2]}}}' --save
# an equirectangular panorama (2:1), .hdr or .png/.jpg
oe import <project> studio.hdr          # -> assets/textures/studio.hdr
oe exec <project> component.set '{"id":"Sky","type":"Skybox","values":{"texture":"assets/textures/studio.hdr","rotation":90}}' --save
# a room that reflects its own walls: a probe in its middle, its box the size of the room
oe exec <project> entity.create '{"name":"Room Probe","components":{"Transform":{"position":[0,1.5,0]},"ReflectionProbe":{"size":[8,3,8]}}}' --save
oe exec <project> probe.state '{"id":"Room Probe","out":"build/probe.png"}'   # look at its picture
```

## Design

Two kinds of sky, one component:

- **Three colors** (`sky` zenith, `horizon`, `ground`): an analytic gradient. No asset, cheap. The
  gradient along a direction is `SampleSky` in `engine/render/RenderScene.h` (C++) and `sky_color` in
  `engine/render/shaders/Shaders.glsl` (GLSL).
- **An image** (`texture`, turned by `rotation`): an equirectangular panorama, prepared once when it
  loads into an `EnvironmentMap` (`engine/render/Environment.h/.cpp`, cached and hot-reloaded by
  `AssetManager::GetEnvironment`):
  - `levels`: level 0 is the image resized to a power of two (at most 2048 x 1024); every further
    level is half the size and blurred by a wider Gaussian lobe (standard deviation
    `0.75 * roughness²` radians, separable along rows — wider in texels toward the poles — and
    columns), down to 4 x 2. A surface of roughness `r` reads level `(r - 0.04) / 0.96 * (levels - 1)`.
  - `sh`: the diffuse light of the whole image as nine spherical-harmonic terms (irradiance / pi, so
    a white image gives white ambient like a flat ambient color), a polynomial of the normal.
  - `.hdr` (Radiance RGBE) keeps light brighter than white (a sun that blooms and gives strong
    highlights); it is converted from linear light to the display colors the renderers light in
    (`pow(x, 1/2.2)`), like the 8-bit decode of the same file. `.png/.jpg` are used as they are.
- Both renderers do the same thing with the same data: the GPU uploads the levels as the mip levels
  of one RGBA16F texture (RGBA8 where half floats cannot be filtered) and reads it with `textureLod`
  (repeat around the horizon, clamp at the poles); the software renderer runs
  `EnvironmentMap::Sample` (bilinear in a level, linear between two). The irradiance polynomial is
  `EnvIrradianceSH` in C++ and the same expression in `mesh_fs`. **Keep the two sides identical** —
  the software renderer is the reference; `SkyboxTextureEnvironment` compares them (mean channel
  difference about 0.2 of 255).
- Lighting (`EnvDiffuse`, `EnvSpecular` in `RenderScene.h`; the environment branch of `mesh_fs`):
  `ambient = diffuse light * albedo + Fresnel * reflection * Skybox.reflections`, times occlusion.
  Shader-graph programs are made from the same shader source, so lit graphs get it too.
- Background (`SampleSkyBackground`; the `sky` program, a fullscreen triangle drawn first in the
  scene pass with depth test always / no write): level 0 of the image, or the gradient plus a soft sun
  disc toward the first directional light. The sun disc is background only (surfaces are lit by the
  directional light itself) and is not drawn with an image, which has its own sun.
- Gathered into `RenderLights::env` (`RenderEnvironment`) by `GatherRenderLights(scene, assets)`.
  No `Skybox` = the previous flat/hemisphere ambient and clear color, byte-for-byte unchanged, so
  scenes without one are not affected and the parity fixtures keep their hashes. An image that does
  not load falls back to the three colors (the error is reported once, and by `asset.info`).

### Reflection probes

A `ReflectionProbe` is a picture of the surroundings taken from the entity's position, used instead
of the sky by the surfaces inside its box.

- **The picture** (`CaptureProbe` in `engine/render/Probes.cpp`): the software renderer draws the
  scene six times from the probe (90 degree faces of `resolution` pixels, one texel wider on every
  side) into float colors before any post effect; the faces are resampled into an equirectangular
  image in world axes (4 x 2 faces) and prepared by `BuildEnvironmentMap` — so a probe is an
  `EnvironmentMap` like an image sky: blurred levels by roughness, irradiance terms. Because the
  software renderer takes it, both renderers and every platform use the same picture. For 128 pixel
  faces in a small scene it costs about 40 ms on a desktop and about 180 ms in a browser (one thread).
- **What it shows**: the scenery, lit as usual (sun with its shadow, point lights, the sky or flat
  ambient, the sky background or the game camera's clear color) but without probes — one bounce, no
  reflections of reflections. Left out (`ProbeShows`): entities that move while the game runs — a
  `RigidBody`, `CharacterBody`, their 2D kinds, an `Animator`, a `VFXSystem` or a `Trail` on the
  entity or an ancestor — which would otherwise stay in the reflections as ghosts where they stood.
  Things only a script moves are not recognised: they are in the picture where they were.
- **When it is taken**: when a frame first needs it, then kept (`AssetManager::ProbePictures`) until
  `AssetManager::InvalidateProbes`. The engine drops the pictures when a scene loads, play starts or
  stops, the game changes scene, and after every edit while stopped (`Engine::Touch`). While
  playing they stay as taken at the start, since scripts change things every frame: call
  `probe.bake` (Lua `game.bakeProbes()`) after the scenery or its lighting changed. The interactive
  editor defers the edits (`DeferProbeEdits`): during a gizmo drag every frame is an edit, so the
  pictures refresh 0.3 s after the edits pause.
- **Which probes a frame uses** (`ActiveProbes`): those whose box, with its fade, is in view; the
  nearest `kMaxProbes` (4) to the eye; smallest box first. Only the engine's own scene uses probes:
  asset thumbnails and effect previews drawn with the same assets do not.
- **Shading** (the probe loop of `Lighting::Shade` and of `mesh_fs`; `ProbeWeight`, `ProbeDir`,
  `ProbeSpecular`, `ProbeDiffuse` in `RenderScene.h`), per pixel: a probe's weight is 1 inside its
  box and falls to 0 over `blend` meters outside it. Probes are visited smallest first, each taking
  its weight of what the ones before it left; the sky (or the flat ambient of a scene without a
  `Skybox`) keeps the rest. A probe replaces the reflected ambient and, with `ambient`, the diffuse
  ambient too — an interior is lit by its own walls, not by the sky.
- **Box projection** (`boxProjection`, on by default): the reflected ray is followed to where it
  leaves the box and the picture is read in the direction from the capture position to that point,
  so the reflection of a wall stays on the wall as the viewer moves. Off = the surroundings are
  treated as infinitely far, for probes outdoors.
- GPU: the probes' pictures are four more textures of the mesh program (`probe0_tex`..`probe3_tex`,
  uploaded like the sky image) and their boxes are in `mesh_lights`.
- `probe.state` lists the probes and whether their picture exists; with `id` and `out` it writes
  the picture as a PNG. In the editor a selected probe shows its box and capture point.

## Milestones

- [x] **M1 — analytic sky lighting (image-based lighting).** `Skybox` component; diffuse ambient from
  the sky above / ground below; specular reflection sampling the sky along the reflection vector,
  blurred toward the sky/ground average with roughness. Both renderers, matched. Test
  `SkyboxEnvironmentLighting`.
- [x] **M2 — skybox background + sun disc.** The sky is drawn along each pixel's view ray as the
  background (`Skybox.background`), with a soft sun disc toward the first directional light
  (`Skybox.sun`, `sunSize`). GPU: a fullscreen triangle (`sky` program in `Shaders.glsl`) drawn first
  in the scene pass, leaving the depth-of-field target at its far clear. Software: a background fill
  from four corner rays.
- [x] **M3 — image environment.** `Skybox.texture` (+ `rotation`): an equirectangular `.hdr` /
  `.png` / `.jpg` as the sky, with blurred levels for rough reflections and spherical-harmonic
  irradiance for diffuse light (see Design). `.hdr` decoding (stb_image's HDR reader enabled),
  `AssetManager::GetEnvironment`, the editor's Create > Rendering > Sky and the texture picker.
  Tests `EnvironmentMapLevelsAndIrradiance`, `SkyboxTextureEnvironment` (software, and software
  against GPU for colors, a `.png` and a `.hdr`). Web and Android runtimes rebuilt.
- [x] **M4 — reflection probes.** `ReflectionProbe` (`size`, `offset`, `blend`, `intensity`,
  `boxProjection`, `ambient`, `resolution`, `nearPlane`, `farPlane`): a picture of the scenery
  taken by the software renderer, box projection, up to four probes per frame blended per pixel, in
  both renderers (see Reflection probes). `probe.bake`, `probe.state`, Lua `game.bakeProbes()`,
  the editor's Create > Rendering > Reflection Probe and box gizmo. Test
  `ReflectionProbeLocalReflections`. Web and Android runtimes rebuilt.

## Limits (current)

- Without a probe the reflection is the global sky for every surface: a wall does not block the sky
  reflection and an interior still reflects the sky. Put a `ReflectionProbe` in each room.
- A probe is a static picture from one point: moving things are not in it (see What it shows), it
  does not follow lights that change during play until `probe.bake`, and reflections are exact only
  for surfaces on the box (a table in the middle of the room is reflected as if it were painted on
  the wall behind it). Its box follows the world axes; the entity's rotation and scale are ignored.
  No reflections of reflections; no screen-space or planar reflections (a mirror, water showing the
  character) — those are separate features.
- At most four probes light one frame (the nearest to the camera); a level with more shows the sky
  where the fifth would be. Pictures are not saved with the scene or the packaged game: they are
  taken when the scene is first drawn (about 40 ms each on a desktop at the default 128 pixel faces,
  several times that in a browser or on a phone, on the thread that draws), so a level with many
  probes hitches once as they come into view; lower `resolution` where reflections are rough anyway.
- The blur of the levels is a Gaussian lobe per roughness, not a split-sum GGX prefilter with a BRDF
  lookup table: the Fresnel term is the same approximation the engine used before. Good to look at,
  not a reference renderer. The separable blur is slightly off right at the poles.
- The background shows the image at no more than 2048 x 1024: soft on a large screen with a narrow
  field of view. Cube maps are not read (convert to equirectangular).
- Distance fog does not tint the sky: with fog on, pick a `fogColor` close to the horizon of the sky.
- A `.hdr` panorama costs about 60 MB of memory while loaded (floats in the asset cache plus the GPU
  copy at 2048 x 1024) and a fraction of a second to prepare at load; `asset.preload` does not
  prepare environments or probe pictures yet (the first frame that shows the scene does;
  `probe.bake` takes every probe's picture at once).
- One environment per scene, no blending between two (day to night: animate the three colors, or
  `intensity`, from Lua; swapping `texture` cuts).
- The three-color sky has no clouds or stars; draw those with scene geometry as `games/RPG` does.
