# Shader graph editor

The editor uses the existing portable `.shader.json` graph format and command-backed
material assets. No platform shader source or new renderer is required.

## Workflow

1. Open **View > Shader Graph**, then **New Shader**, or double-click a shader/material
   in Assets. **Edit Material** in a MeshRenderer inspector opens its material and graph.
2. **Add Node** offers all 25 portable operations. Drag an output pin to an input pin;
   right-click an input to disconnect it. Connect a node to the **Surface Output** card's
   `color`, `emissive`, `normal`, or `offset` input. Cycles are rejected immediately.
3. Click a node's header or body to edit its constant, uniform, or swizzle in **Node Properties**.
   **Uniform Defaults** adds named scalar defaults; its context menu switches scalar/vector
   values or removes a uniform. A uniform node chooses one of the declared names.
4. Move cards by dragging their headers or bodies, pan with the middle mouse button, and zoom with the wheel.
   **Arrange Nodes** lays out dependency levels; **Fit Graph** shows the whole canvas.
   Node positions are workspace layout, not serialized into the portable graph file.
5. Enter a `.shader.json` project path and **Save Shader** (Ctrl+S). The editor topologically
   sorts the nodes and remaps every argument/output, so connections may go in either
   direction in the workspace. All inputs must be connected and `color` must be assigned.
   Engine validation also enforces the 48-node/8-uniform limits and vertex-stage restrictions.
   An invalid graph cannot replace the last valid file. Graph undo/redo is local to the
   workspace (buttons or Ctrl+Z/Ctrl+Y) and does not undo scene edits.
6. Choose the `.mat.json` path in the next toolbar row. **Material** edits base color,
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

The live sphere swatch evaluates the CPU surface graph at time = 1, with a white texture,
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

## Status

- [ ] Linux/EGL editor build and visual check of the Shaders workspace.

