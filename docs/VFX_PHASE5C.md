# VFX phase 5c: expression value nodes

Value nodes are an authoring view of the bounded expression compiler, not another evaluator.
The effect document retains expression text only; node positions live in project `.oe/` sidecars.
Edits compile back into source and use the shared VFX document commands and history.
Unsupported conversions and invalid connections preserve the source document.

## Editor workflow

Open a `.vfx.json` in Effects, enable **Value Nodes**, then choose its emitter, Birth/Update stage
and channel. **Structure Graph** remains the block/reference view; Shaders remains the material editor.
An unassigned channel displays a starter graph and keeps authored values until nodes are applied.

- Drag a header to move its card. Middle mouse pans, wheel zooms; Fit Graph and Auto Arrange reset framing.
  Header/body text scales with canvas zoom; the selected node's properties remain full size.
- Add Value Node offers constants, particle inputs, parameters, scalar/vector math, vec3, component
  reads and curves. Select a node for its literal/input/axis and indexed argument controls.
- Connect output pins to argument pins or the green channel output. Right-click an input to disconnect.
  Complete, valid edits apply through `vfx.edit_replace`; malformed or incomplete topology stays
  pending with an error and does not alter the shared source document. Reset Nodes discards pending topology.
- Literal text accepts unsigned decimals or `pi`; Negate represents negative values. Numeric dragging
  coalesces into one document undo step per drag. Enter commits a literal edit. Remove Node deletes
  a node and leaves affected inputs pending until reconnected.
- Curve inputs are time followed by ascending constant age/value pairs. Add/Remove Key edits its
  argument pairs; wire constant ages and scalar/vector values, matching the expression language.
- Apply Nodes explicitly validates and commits the current graph. Source text can also be edited and
  applied in the same view. Undo/Redo and remote `vfx.set` edits refresh nodes from the shared document.
  Scalar/vector parameter type changes refresh compiler-derived node types. String/bool parameters
  continue working in the effect document and are excluded from expression parameter pickers.
- Save Effect saves the last validated source document. Incomplete pending topology is transient and
  is not saved. Positions, pan and zoom live in `.oe/vfx-layout/`; moving nodes does not dirty the asset
  or create document history. Layouts use block/stage/channel/compiler-index keys; after structural
  text edits, Auto Arrange can reframe retained positions.

```text
oe editor samples/VFXLab --effects effects/ice_pillar.vfx.json --effects-values
```

The screenshot flag selects Update custom3, showing the pillar dissolve curve. Other channels are
selected through the toolbar. Enable Value Nodes interactively to edit any emitter/channel.

![Korean value-node workspace](images/effects-value-nodes-ko.png)

## Command contract

`vfx.schema.expressions.valueGraph` describes operations, inputs and limits. Read-only conversions:

```json
{"command":"vfx.expression_graph","args":{"source":"age*2","type":"scalar"}}
{"command":"vfx.expression_source","args":{"type":"scalar","graph":{"format":"oe.vfx.expression_graph","version":1,"nodes":[{"op":"input","name":"age","args":[]},{"op":"constant","literal":"2","args":[]},{"op":"multiply","args":[0,1]}],"output":2}}}
```

Both return `source`, a compiler-derived typed `graph`, output `type` and compiled instruction count.
Export also returns `nodeTypes` in the supplied graph's node order, including disconnected nodes.
Graph nodes have `op`, integer `args`, plus `literal`, `name` or `axis` where applicable; `type` from
conversion is advisory metadata, never trusted as a type declaration. Parameters use an optional
`params` object of at most 32 numeric scalar/three-vector defaults. Node and output indices refer to
this transient node array, with zero-based X/Y/Z axis indices for component reads.

Graphs allow 1..64 nodes, depth at most 32. References, cycles and arities are checked, including
unreachable nodes, then every node and the channel output are compiled by the runtime compiler.
Sharing nodes expands to expression text; the resulting 1024-byte/64-instruction/depth limits still
apply, and full-document replacement additionally checks emitter/effect/step budgets. No graph
instructions bypass those limits and there is no graph interpreter in the editor.

Numeric literal spelling, including tiny decimals and `pi`, is captured during compilation so
conversion cannot introduce decimal-rounding drift. Recompiled operations, types, arguments,
values and names must match exactly before a text conversion is accepted. Text layout/whitespace
can normalize when a node edit is applied; layout-only changes do not rewrite source.

Component reads require a named vector input or parameter, matching the current text grammar.
The serializer uses explicit parentheses; valid text whose canonical expansion exceeds source/depth
limits is refused with `unsupported_expression_graph`. Keep it in text view. Invalid graph fields,
cycles, missing references, wrong types or budgets are rejected with a hint. Conversion commands
never mutate the scene or isolated draft: an agent applies returned source through existing `vfx.set`
or `vfx.edit_replace`, then uses the shared undo/save flow. No typed tree or bytecode goes into assets.

## Verification

- `VfxExpressionValueGraphCommands` covers scalar/vector/parameter/axis/decimal round trips,
  equivalent evaluated values at four ages, cycles, invalid indices/names/ops, output-type rejection
  and explicit refusal of an otherwise valid deep canonical expansion. Scene/history remain unchanged.
- `VfxAcceptanceExpressionGraphsRoundTrip` converts every Birth/Update channel in all six 5b+
  acceptance assets and verifies identical results after graph/source round trips.
- `NativeEditorEffectsValueNodes` drives real mouse events for layout-only dragging, coalesced constant
  edits, undo/redo, cycle/type refusal, palette creation, output rewiring, remote source edits and
  sidecar reload in a fresh editor. Its fixture also has a nonnumeric parameter.
- Korean curve and English event-driven workspaces are inspected on D3D11. Existing structure and
  Shader canvas interactions, translations and the full suite remain regression gates.
- Runtime simulation semantics did not change. The compiler now retains literal spelling for authoring,
  and common command registration adds conversion commands; Web/Android players are refreshed below.
  Browser GPU, Android devices and Linux/EGL execution remain unverified.

Real CLI graph export -> isolated draft edit -> undo/redo -> preview/check -> discard passed.
The unsaved 128px four-tile preview hash is `4d9aac489addc053` (0.2/0.6/1/2.8s).
All 244 Node/WASM tests pass, including the same six-effect and expression-math reference hashes.
Web and Android arm64-v8a/x86_64 players rebuilt; runtime README files record their SHA256 hashes.

## Implementation status (work log)

- [x] Bounded text/graph conversion commands, typed operations and explicit conversion refusal.
- [x] Value canvas, palette, pins, channel output and constant/input/curve properties.
- [x] Shared source editing, drag coalescing, undo/redo and remote document refresh.
- [x] Layout sidecars independent of effect data and history.
- [x] Compiler/command round trips, six acceptance assets and native interaction tests.
- [x] Final Windows Release full suite: 299 tests pass, zero failed checks and no new C++ warnings.
- [x] Node/WASM full suite: 244 tests pass; existing unused ProcessFixtureDir warning remains.
- [x] Korean/English D3D11 screenshots and Web/Android arm64-v8a/x86_64 player rebuilds.
- [ ] Other desktop GPU backends, browser GPU and Android device execution.
