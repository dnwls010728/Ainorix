# GPU renderer performance

How the GPU renderer keeps frame time low, how to measure it and what is still open.
The software renderer is the reference for hashes and tests and is not covered here
(see [RENDERING.md](RENDERING.md#performance)).

## Measuring

`tools/perf/bench.sh <project> [frames]` plays the project in the GPU editor headless,
reads every frame back (so GPU work counts) and prints the mean wall time per frame,
without editor start-up. It includes the simulation and the editor UI; an empty
project measures about 0.1 ms. Run it twice: results vary by about 0.3 ms.

`render.screenshot {renderer:"gpu"}` reports `stats.ms`, the CPU time the renderer
spent recording the frame (not the GPU time).

Reference machine for the numbers below: Intel i7-13700K, NVIDIA RTX 4070 Ti,
Windows 11, D3D11, 1600×900 editor window.

## Implementation status (work log)

Baseline `games/RPG` (after the shadow-fit fix, commit b816b35): **~16.0 ms/frame**.
Measured split: renderer CPU ~2 ms (gather 0.7, scene pass 0.6, shadow pass 0.3,
depth-of-field depth pass 0.3), simulation ~1.4 ms, the rest GPU. Without shader
graphs the frame takes ~4.2 ms: the per-pixel graph interpreter is the main cost.

Order: small independent steps first, the graph compiler last. Each step is
verified with `oe_tests` (incl. `GpuRendererMatchesSoftware`), the RPG benchmark
and screenshots, and the web/Android runtimes are rebuilt when the step is done.

- [x] 1. Depth of field reads the scene pass's depth: the scene pass writes view
      depth to a second color target (MRT) instead of a separate depth pass that
      draws the scene again. Falls back to the depth pass where multisampled
      RGBA16F targets are not renderable. RPG 16.0 → 13.1 ms; the RPG frame is
      unchanged (mean channel difference 0.014). WebGL2/GLES3 execution of the
      MRT pass is unverified until the runtimes are rebuilt and run.
- [x] 2. Smaller per-draw uniforms: the 64-joint palette (4 KB) has its own
      uniform block (`mesh_vs_skin`, `pos_vs_skin`), uploaded for skinned items and
      once per pipeline switch otherwise. Desktop D3D11 barely changes (RPG ~13.1 ms,
      renderer CPU ~1.7 ms); the saving is meant for GLES3/WebGL2, where every
      upload is a `glUniform4fv` call (unmeasured there). The palette is still
      declared, so the vertex stage still declares ~380 uniform vectors (WebGL2
      guarantees 256): a skinned/static shader split would be needed to lower that.
- [ ] 3. Frustum culling of the scene pass (camera) and the shadow pass (light),
      with displaced surfaces widened by their `maxOffset` margin.
- [ ] 4. Instanced sprites: consecutive draws of the same quad and material are
      drawn with one instanced call.
- [ ] 5. Shadow map fitted to the camera view (shadow distance) instead of the
      whole scene.
- [ ] 6. Shader graphs compiled to HLSL / GLSL ES at load time (one program per
      graph), the interpreter kept as fallback when compilation fails.
