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

- [ ] 1. Depth of field reads the scene pass's depth: the scene pass writes view
      depth to a second color target (MRT) instead of a separate depth pass that
      draws the scene again. Falls back to the depth pass where multisampled
      RGBA16F targets are not renderable.
- [ ] 2. Smaller per-draw uniforms: the 64-joint palette (4 KB) is uploaded only
      for skinned meshes. Also keeps the vertex stage under WebGL2's minimum of
      256 uniform vectors.
- [ ] 3. Frustum culling of the scene pass (camera) and the shadow pass (light),
      with displaced surfaces widened by their `maxOffset` margin.
- [ ] 4. Instanced sprites: consecutive draws of the same quad and material are
      drawn with one instanced call.
- [ ] 5. Shadow map fitted to the camera view (shadow distance) instead of the
      whole scene.
- [ ] 6. Shader graphs compiled to HLSL / GLSL ES at load time (one program per
      graph), the interpreter kept as fallback when compilation fails.
