# WuwaToon — portable toon shader study

A runnable OwnEngine test project with an original procedural mannequin,
toon/PBR/normal comparisons and editable `.shader.json` materials. This is a
community-reference-inspired rendering experiment, not an official Wuthering
Waves shader, character or game asset pack.

```bat
build\bin\oe.exe editor samples/WuwaToon
build\bin\oe.exe run samples/WuwaToon
build\bin\oe.exe render samples/WuwaToon --renderer gpu --out build/wuwa-toon.png
build\bin\oe.exe package samples/WuwaToon --web --out dist/WuwaToon-web
build\bin\oe.exe serve dist/WuwaToon-web
```

In the editor, press Play and focus the Game view to use the controls. The
buttons also work with mouse/touch in the packaged player. Entering the project
does not require an asset download, Python, a shader compiler or a rebuild.

| Key / button | Result |
|---|---|
| 1 / Toon | Toon materials; stored outline setting applies |
| 2 / PBR | Standard materials on the identical geometry; outlines hidden |
| 3 / Normals | World-space normal visualization; outlines hidden |
| O / Outline | Toggle the stored outline setting for Toon mode |
| P / Turntable | Start/pause deterministic 24-degree-per-second rotation |
| R / Reset | Front pose, Toon mode, outlines enabled, rotation stopped |

The three spheres stay in their PBR/Toon/Normals modes as a visual reference.
The studio camera and graph lighting vectors are intentionally fixed. Rotate
the character rather than the camera when testing the view-angle rim.

## Editable shader

`materials/toon.shader.json` has 28 nodes and eight uniforms:

- `lightDir`: normalized world-space direction toward the studio light, w = 0.
- `shadowCut`: threshold for the normal/light dot product.
- `shadowTint`: multiplicative RGBA shadow color; keep alpha at 1.
- `cameraPos`: actual world-space camera position, w = 1.
- `rimStart`: threshold for `1 - dot(normal, viewDirection)`.
- `rimColor`: additive rim RGB, alpha = 0.
- `halfDir`: normalized fixed studio light/view half-vector, w = 0.
- `specColor`: additive quantized highlight RGB, alpha = 0.

Skin, hair, clothes and accessories override selected uniforms independently.
The graph computes a hard diffuse band, tinted shadows, a narrow view-angle rim
and a thresholded specular patch. Materials use `unlit:true` so the engine does
not add a second continuous PBR lighting layer. Geometry still casts onto the
standard lit stage; the toon surface does not receive cast-shadow darkening.
Both renderers run the same graph. No arbitrary HLSL/GLSL or engine extension
is required. `materials/normals.shader.json` visualizes the graph input normals.

Validate assets through `shader.check`, `asset.info` and `script.check`.
Change per-material values with `material.set`; dependency hot reload applies
the edits. A changed camera or studio light needs matching graph uniforms.

## Public references and provenance

Technical reference, reviewed 2026-10-03:
[HoyoToon Wuthering Waves common shader](https://github.com/Hoyotoon/HoyoToon/blob/d9e5ca2f312bf16fba89dee67d32c08b482dcda4/Shaders/Wuthering%20Waves/Include/HoyoToonWutheringWaves-common.hlsl).
It documents community implementations of shadow/ramp, specular and rim
effects. HoyoToon is a community project; it is not an official Kuro source.
Its [repository license](https://github.com/Hoyotoon/HoyoToon/blob/d9e5ca2f312bf16fba89dee67d32c08b482dcda4/LICENSE)
is GPL-3.0. No HoyoToon source or game model/texture is bundled or ported here.
This graph is authored independently from elementary dot/step/mix operations.

All geometry, palettes, scene assets and scripts in this directory are original
OwnEngine sample content. `tools/make_lab.py` regenerates the committed GLBs,
graphs, materials and scene with Python's standard library. It does not download
third-party content. Face SDF masks, depth-buffer rims, material-ID control maps,
hair anisotropy, stencil hair/face layering and original-game tone mapping are
not implemented; this sample demonstrates the portable graph subset honestly.

Verification and platform follow-ups: [docs/TOON.md](../../docs/TOON.md).
