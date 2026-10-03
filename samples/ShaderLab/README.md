# ShaderLab

Run `oe run samples/ShaderLab`. The left panel evaluates animated UV bands with
HDR emission; the right panel evaluates a striped alpha mask, including its
ground shadow. Press 1 for neutral output, 2 for HDR/bloom.

Edit `materials/pulse.shader.json` while the editor/player runs to hot reload the
graph. Change `frequency` or `tint`; both material and graph dependencies reload.
Per-material overrides are available through `material.set` with `shaderUniforms`.

Capture a fixed time with `oe render samples/ShaderLab --frames 60 --out build/shader.png`.
Add `--renderer gpu` to compare. Package with `oe package samples/ShaderLab --web`;
the graphs and materials travel in game.pak. See docs/POSTPROCESS.md for the graph
operations and limits.
