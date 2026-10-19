# Shader graph editor

The editor uses the existing portable `.shader.json` graph format and command-backed
material assets. No platform shader source or new renderer is required.

## Workflow

1. Open **View > Shader Graph**, then **New Shader**, or double-click a shader/material
   in Assets. **Edit Material** in a MeshRenderer inspector opens its material and graph.
2. **Add Node** opens a searchable list grouped by kind (Inputs, Values, Texture, Math, Vector) with a line on what
   each of the operations does. Right-clicking or double-clicking the empty canvas opens the same list and puts the
   node there. Drag an output pin to an input pin; right-click an input to disconnect it. Connect a node to the
   **Surface Output** card's `Color`, `Emissive`, `Normal` or `Offset` input. Cycles are rejected immediately.
3. Cards are colored by kind (inputs green, values amber, texture purple, math blue, vector teal, the output red) and
   wires take the color of the node they leave. A card shows the operation's name, its number in the graph (`#12`),
   named pins (`Value / Min / Max`, `A / B / Blend`) and what it holds: a number, a color swatch, a uniform's name, a
   swizzle as letters. Hovering a card says what the operation does. Clicking one selects it: its wires are drawn
   brighter and the others step back, and **Node Properties** edits its constant (number or color/vector, with a
   color picker), uniform or swizzle (one choice per output channel). **Delete** removes the selected node.
   **Uniform Defaults** adds named scalar defaults; its context menu switches scalar/vector values or removes a
   uniform. A uniform node chooses one of the declared names.
4. Move cards by dragging them, move the view by dragging the empty canvas (or with the middle mouse button), and
   zoom with the wheel; the zoom is shown at the bottom right of the canvas, the mouse hints at the bottom left.
   **Arrange Nodes** puts every node just left of what reads it, level with the pins it feeds, so inputs and
   constants are not stacked in one column far from their use. **Fit Graph** shows the whole graph. A graph too
   large to read when fitted opens at 70 % on the Surface Output instead.
   Node positions are workspace layout, not serialized into the portable graph file.
5. Enter a `.shader.json` project path and **Save Shader** (Ctrl+S). The editor topologically
   sorts the nodes and remaps every argument/output, so connections may go in either
   direction in the workspace. All inputs must be connected and `color` must be assigned.
   Engine validation also enforces the 128-node/8-uniform limits and vertex-stage restrictions.
   An invalid graph cannot replace the last valid file. Graph undo/redo is local to the
   workspace (buttons or Ctrl+Z/Ctrl+Y) and does not undo scene edits.
6. The toolbar has the shader file and **Save Shader** in its first row (amber with a `*` while there are unsaved
   changes), the material in the second, and the graph tools with the state of the graph in the third: a green
   "No problems" with the node count, or the first line of what stops the graph from being saved (the whole text is
   in the tooltip and at the top of the properties). Choose the `.mat.json` path in the second row. **Material** edits base color,
   roughness, metallic, unlit, opacity, alpha mode, base texture and per-material uniforms.
   **Create Material** saves the graph and creates a material referencing it. Opening an
   existing material uses **Save Material** and preserves fields not exposed by this UI.
   New paths never overwrite existing assets; proposed names avoid existing files.
7. Select an entity with MeshRenderer and **Apply to Selected Mesh**. It saves the assets
   and applies the material through `component.set` as one scene undo step. Previous
   per-entity shader uniform overrides are cleared to avoid referring to a different graph.

Switching documents or quitting with unsaved graph/material changes offers save, discard,
or cancel. Hiding the panel retains its workspace. Asset writes use `shader.create` and
`material.create`/`material.set`; file writes are independent of scene undo, as in the CLI.

## Preview and limits

Properties are rows with the label on the left. The live sphere swatch (as large as the panel allows) evaluates the CPU surface graph at time = 1, with a white texture,
the material base color and uniform overrides. It visualizes color/emission/alpha; it does
not simulate lighting, textures, replaced normals, or vertex displacement. The saved
material thumbnail below the properties uses the existing lit asset-preview renderer.
The Scene/Game views show the actual material on the selected mesh after assignment.

This edits the engine's portable surface graphs, rather than arbitrary GLSL/HLSL files or
post-processing passes. Keep `normal`/`offset` vertex and fragment restrictions described
in [POSTPROCESS.md](POSTPROCESS.md). No runtime C++ or shader backend changed here.

Headless inspection:

```bat
oe editor games/RPG --shader materials/water.shader.json --screenshot build/shader.png --lang ko
```

Automation finds the workspace's controls and cards through `NativeEditor::ShaderControlRect(name)` (`"add"`,
`"arrange"`, `"save-material"`, `"node:<i>"`, `"out:<i>"`, `"in:<i>:<pin>"`, `"surface:color"`, `"palette:<op>"`, ...)
instead of assuming where things are drawn.

The Add Node list includes the math nodes `pow`, `smoothstep`, `noise`, `fbm` and `length`, the `ambient`, `viewDirection`
and `objectPosition` inputs and the light-stage inputs `lightVector`, `lightColor` and `lightAttenuation`; the
surface output node has a `Light` pin next to Color/Emissive/Normal/Offset (see [POSTPROCESS.md](POSTPROCESS.md#light-stage-custom-lighting)).

### Post graphs

**New Post Shader** starts a post graph (`"stage": "post"`, [POSTPROCESS.md](POSTPROCESS.md#post-graphs)), and opening
a post `.shader.json` from Assets edits it in the same workspace. The output card is **Post Output** with Color only,
the Add Node list offers the post inputs `sceneColor`, `sceneBlur` and `texel` instead of the surface ones, and
the limit is 128 nodes. **Injection Point** in the graph properties chooses After Tone Mapping (the default, on
the display image) or Before Tone Mapping (`"injection": "beforeToneMapping"`, on the HDR scene before bloom). The
preview runs the graph over a built-in test picture (sky, ground, a lit ball, color
bars) the way the software renderer runs it on the image. **Apply to Selected Camera** saves the graph and sets
`PostProcess.shader` on the selected entity (it needs a PostProcess). Check:
`oe editor <project> --screenshot shot.png --shader shaders/watercolor.shader.json --select "Main Camera"`.

Post graphs expose **Injection Point** in their properties: **After Tone Mapping** preserves the display-image pass; **Before Tone Mapping** processes HDR scene color before bloom and the display conversion. The selection is part of the graph document and shares its undo and save.

## Status

- [ ] Linux/EGL editor build and visual check of the Shaders workspace.
- [x] Post graphs: post mode with its own nodes, a test-picture preview and Apply to Selected Camera (checked
      with `oe editor --screenshot` on games/Kongal's watercolor graph).
- [ ] Visual check of the new math nodes in the editor (`oe editor --screenshot`); not run in this change.

