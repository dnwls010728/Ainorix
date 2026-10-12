# Environment & image-based lighting

A long feature (DESIGN.md §5). It gives a scene an environment — a sky that lights surfaces, that
smooth/metallic surfaces reflect and that is drawn behind the scene — so PBR materials stop looking
flat. Built in milestones; each milestone is a commit.

The environment is the `Skybox` component (one per scene; Create > Rendering > Sky in the editor).
See [RENDERING.md](RENDERING.md) for how it feeds lighting, and `docs/API.md` for its fields.

```bash
# three colors
oe exec <project> entity.create '{"name":"Sky","components":{"Skybox":{"sky":[0.2,0.4,0.8],"horizon":[0.9,0.7,0.5],"ground":[0.2,0.2,0.2]}}}' --save
# an equirectangular panorama (2:1), .hdr or .png/.jpg
oe import <project> studio.hdr          # -> assets/textures/studio.hdr
oe exec <project> component.set '{"id":"Sky","type":"Skybox","values":{"texture":"assets/textures/studio.hdr","rotation":90}}' --save
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
- [ ] **M4 — reflection probes.** Local captures / box projection so interiors reflect their own room
  rather than the global sky.

## Limits (current)

- The reflection is the global sky for every surface: no local reflections, no occlusion of the
  environment by nearby geometry (a wall does not block the sky reflection; an interior still
  reflects the sky), no parallax. Ambient occlusion textures are the only thing that darkens it. M4.
- The blur of the levels is a Gaussian lobe per roughness, not a split-sum GGX prefilter with a BRDF
  lookup table: the Fresnel term is the same approximation the engine used before. Good to look at,
  not a reference renderer. The separable blur is slightly off right at the poles.
- The background shows the image at no more than 2048 x 1024: soft on a large screen with a narrow
  field of view. Cube maps are not read (convert to equirectangular).
- Distance fog does not tint the sky: with fog on, pick a `fogColor` close to the horizon of the sky.
- A `.hdr` panorama costs about 60 MB of memory while loaded (floats in the asset cache plus the GPU
  copy at 2048 x 1024) and a fraction of a second to prepare at load; `asset.preload` does not
  prepare environments yet (the first frame that shows the scene does).
- One environment per scene, no blending between two (day to night: animate the three colors, or
  `intensity`, from Lua; swapping `texture` cuts).
- The three-color sky has no clouds or stars; draw those with scene geometry as `games/RPG` does.
