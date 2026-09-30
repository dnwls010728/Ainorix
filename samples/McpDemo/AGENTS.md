# Working on this OwnEngine project (for AI agents)

- Scenes are plain JSON in `scenes/`. You may edit them directly, but prefer the engine API so values are validated.
- Inspect: `oe exec . scene.summary` / `oe exec . component.types`.
- Edit:    `oe exec . entity.create '{"name":"Box","components":{"MeshRenderer":{}}}' --save`.
- Verify:  `oe render . --out shot.png` then look at the PNG; `--frames 120` simulates 2 seconds first.
- Live:    `oe mcp .` exposes every command as an MCP tool (screenshots come back as images).
- Human:   `oe editor .` opens the editor (it serves the same API on http://127.0.0.1:7777; attach with `oe mcp --connect 7777`).
