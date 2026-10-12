# Environment & image-based lighting

A long feature (DESIGN.md §5). It gives a scene an environment — a sky that lights surfaces and that
smooth/metallic surfaces reflect — so PBR materials stop looking flat. Built in milestones; each
milestone is a commit.

The environment is the `Skybox` component (one per scene). See [RENDERING.md](RENDERING.md) for how it
feeds lighting, and `docs/API.md` for its fields.

## Design

- One analytic sky: three colors (`sky` zenith, `horizon`, `ground`), an `intensity` and a
  `reflections` strength. No texture or HDR file (deterministic, no assets, cheap, identical in both
  renderers). A textured / HDR environment is a later milestone.
- The sky gradient along a direction is `SampleSky` in `engine/render/RenderScene.h` (C++) and
  `sky_color` in `engine/render/shaders/Shaders.glsl` (GLSL). **They must stay identical** — the
  software renderer is the reference and `GpuRendererMatchesSoftware` compares the two.
- Gathered into `RenderLights::env` (`RenderEnvironment`) by `GatherRenderLights`. Absent = the
  previous flat/hemisphere ambient, byte-for-byte unchanged, so scenes without a `Skybox` are not
  affected and the parity fixtures keep their hashes.

## Milestones

- [x] **M1 — analytic sky lighting (image-based lighting).** `Skybox` component; diffuse ambient from
  the sky above / ground below; specular reflection sampling the sky along the reflection vector,
  blurred toward the sky/ground average with roughness. Both renderers, matched. Test
  `SkyboxEnvironmentLighting`. (The sky is not yet drawn as the background.)
- [x] **M2 — skybox background + sun disc.** The sky is drawn along each pixel's view ray as the
  background (`Skybox.background`), with a soft sun disc toward the first directional light
  (`Skybox.sun`, `sunSize`). GPU: a fullscreen triangle (`sky` program in `Shaders.glsl`) drawn first
  in the scene pass, depth test always / no write, leaving the depth-of-field target at its far clear.
  Software: a per-pixel background fill. `SampleSkyBackground` (RenderScene.h) and `sky_fs` match.
  The sun disc is background only — surfaces are still lit by the directional light, so it is not
  double-counted in the reflection.
- [ ] **M3 — textured / HDR environment.** An equirectangular or cubemap image as the environment,
  with a prefiltered mip chain for rough reflections and an irradiance term for diffuse ambient.
- [ ] **M4 — reflection probes.** Local captures / box projection so interiors reflect their own room
  rather than the global sky.

## Limits (current)

- Analytic gradient only; no textured/HDR sky, no sun disc, no clouds (M2/M3).
- The reflection is the global sky for every surface: no local reflections, no occlusion of the
  environment by nearby geometry (a wall does not block the sky reflection), no parallax.
- Roughness blur is a cheap lerp toward the sky/ground average, not a true prefiltered convolution.
- One environment per scene.
