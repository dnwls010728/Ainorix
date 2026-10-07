# VFX phase 4b: Effects structure graph

Effects now offers a structure graph of the same command-backed document used by the field view.
Usage and wire semantics are in [VFX.md](VFX.md#effects-structure-graph). The graph carries existing
references; expression evaluation and per-particle value nodes remain the optional phase 5.

The shared EditorNodeCanvas extracts pan/zoom, card dragging, pin hit areas, fit and wire drawing from
Shaders while preserving its document history, ordering and validation. Effects uses header-only
card dragging so inline fields and curves can capture their own input. Layout and palette references
live in atomically saved `.oe/vfx-layout/` sidecars, separately from effect content and document undo.

## Verification

- Warning-free Windows Release build; complete native suite: 282 tests, zero failed checks.
- EditorVfxReferencesMatchCommands compares material, parameter, death-trigger and ribbon-anchor
  graph patches with equivalent `vfx.set` edits, including canonical documents and undo. Invalid
  type validation preserves the shared draft, and parameter disconnect cannot leave an orphan.
- NativeEditorEffectsGraph uses actual window events to move a node, connect a parameter, reject a
  wrong-type connection, undo, create an emitter and expand its Curves module. It checks that layout
  changes preserve asset dirty state and `vfx.diff`, and reloads a sidecar in a fresh editor instance.
- Existing Shader workspace dragging/linking/material tests and EditorTranslations remain gates.
  Korean Effects graph and ShaderLab pulse graph screenshots were inspected on D3D11.
- No runtime simulation, renderer or player source changed. Phase 4 Web/Android prebuilts remain
  current; this workspace is linked only into the desktop editor. Linux/EGL and other desktop editor
  backends were not executed. Browser WebGL2 and Android device checks remain phase 4 follow-ups.

## Implementation status (work log)

- [x] Shared canvas and `.oe/` sidecar storage decision.
- [x] Structure nodes, fixed expandable modules, typed parameter/budget edits, reference wires,
      material navigation and compact independent preview.
- [x] Final warning-free build and complete native suite (282 tests, zero failed checks).
- [ ] Other desktop GPU backends and device runs.
