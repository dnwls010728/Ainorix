# NetChase

A small third-person multiplayer race on the authoritative network model. Up to four
players run around a walled arena with pillars; touching the gold crystal scores and the
first to five wins. The camera sits behind your own player.

Controls: W/S run forward/back, A/D strafe, the mouse turns, Space jumps, R starts a new
round. The mouse is captured when you first move or when a match starts; Escape frees the
cursor (offline, click once more to reach the lobby buttons). The key actions are also
on-screen buttons, but turning needs a mouse: set the Camera script's `mouseLook` param to
false for touch-only builds.

Run `build/bin/oe.exe editor samples/NetChase`, choose Players 2 (up to 4), then Play;
switch Game peers to control each player. Players 1 is offline practice.

For two native players, run `build/bin/oe.exe run samples/NetChase` twice: Host on one,
Join on the other, Ready on both, then Start on the host. Default binding is loopback
(TCP 127.0.0.1:7778); see [the network sample guide](../../docs/NETWORK_SAMPLES.md) for
LAN, dedicated-server and browser setups, which work the same way as NetArena.

Files: `scripts/player.lua` (movement from declared inputs, predicted on the owner),
`scripts/camera.lua` (local follow camera and mouse look; the turn is stored with
`input.setLook` and travels in the input stream as the `LookX` network axis),
`scripts/game.lua` (server-only scoring), `scripts/lobby.lua` (host/join/ready/start buttons). The pillar list in `player.lua`
mirrors the Pillar entities in the scene.

## Toon look

Players are an original procedural mannequin drawn with `materials/toon.shader.json`: a
hard light band with a tinted shadow, a view-angle rim, a quantized highlight and
inverted-hull outlines (the technique studied in [samples/WuwaToon](../WuwaToon/README.md);
a community-reference-inspired experiment, not an official Wuthering Waves shader or
asset). The graph declares the reserved `cameraPosition` and `lightDirection` uniforms,
which the renderer fills every frame, so the rim and highlight follow the third-person
camera. The coat is the prefab root mesh and is tinted by the replicated player colour;
pillars and the crystal use `materials/prop-toon.mat.json`. Toon surfaces cast shadows but
are not darkened by them. `python tools/make_art.py` regenerates the models, graph,
materials and prefab (Python standard library only; not needed to run the sample).
