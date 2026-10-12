# Navigation mesh

Where characters can walk, and the shortest way between two points: enemies that go around walls, click-to-move,
NPCs that patrol. The engine bakes a navigation mesh from the scene's static geometry and answers path queries on
it; walking along a path is a script (the `oe.nav` module does it).

```bash
oe exec build/demo nav.state                                       # what was baked: polygons, area, sources
oe exec build/demo nav.path '{"from":[-8,0,-8],"to":[-8,0,8]}'     # corner points around the walls
oe exec build/demo render.screenshot '{"navmesh":true,"path":"nav.png"}'   # the mesh drawn over the scene
oe editor build/demo --screenshot build/editor.png --navmesh       # the Scene view's NavMesh overlay
```

## What becomes ground

Nothing has to be set up: the first query bakes the mesh from every **static `Collider`** of the scene (box, sphere,
capsule) with the default agent. Tops an agent can stand on become walkable, everything else is an obstacle.

Left out of the bake: triggers (`isTrigger`), anything with or below a `RigidBody`, `CharacterBody` or `Animator`
(things that move), skinned models, hidden meshes, and entities with `NavModifier {mode: "ignore"}` (their
children too). 2D colliders and tilemaps are not sources.

Add a `NavMesh` component to one entity to change the settings (the first in id order counts):

| Field | Default | Meaning |
|---|---|---|
| `agentRadius` | 0.4 | The mesh keeps this far from walls and edges |
| `agentHeight` | 1.8 | Lower passages are not walkable |
| `maxSlope` | 45 | Steepest walkable slope, degrees |
| `stepHeight` | 0.3 | Highest ledge stepped up or down (stairs, kerbs) |
| `cellSize` | 0.2 | Sampling size in meters; about `agentRadius / 2`. Smaller follows walls more closely and bakes slower |
| `minRegion` | 1 | Walkable islands below this area (m2) are dropped, e.g. the tops of crates |
| `source` | `colliders` | `colliders`, `meshes` (static `MeshRenderer` models: terrain without colliders) or `all` |
| `collect` | `all` | `children` = only the NavMesh entity and those below it |

`NavModifier {mode}` on an entity (and its children): `ignore` leaves it out (sky domes, decoration when baking from
meshes), `unwalkable` keeps it as an obstacle nobody stands on (water, lava).

One mesh serves one agent size. Match `NavMesh.agentRadius/agentHeight/stepHeight/maxSlope` to the `CharacterBody`
of the characters that use it.

## When it is baked

- **Stopped (editing)**: the mesh follows the scene. A query after an edit that changed the sources bakes again.
- **Playing**: baked at the first query of the play session (and after `game.loadScene`) and then kept, because
  scripts move things every frame. When the level itself changes (a door opens, a bridge falls) call `nav.bake` /
  Lua `nav.bake()`. Bake time grows with the area covered divided by `cellSize` squared: `games/RPG` baked from its models
  with the 700 m sky and mountain rings left in (3025 tiles) took 2.3 s on the development PC, so bake at level
  start rather than during action, and mark far decoration `NavModifier {mode: "ignore"}`.
- The mesh is derived data: it is not saved in scene files or snapshots. The same geometry and settings give the
  same mesh and the same paths.

A world larger than 16384 tiles of 64 cells (about 1.6 km a side at `cellSize` 0.2, less when one side is longer)
is refused with a hint to raise `cellSize`.

## Commands

| Command | Result |
|---|---|
| `nav.state` | `{baked, polygons, vertices, tiles, area, boundsMin, boundsMax, colliders, meshes, triangles, warnings, settings}`; `error` + `hint` when nothing could be baked |
| `nav.bake` | Bakes again now and returns the same report; error `no_navmesh` when nothing is walkable |
| `nav.path {from, to, range?}` | `{found, partial, points: [[x,y,z],...], length}`. Both ends are snapped to the mesh within `range` (default 2 m, twice that vertically). `partial`: the goal cannot be reached and the path ends as near as it gets. `found: false` comes with `reason` |
| `nav.nearest {point, range?}` | `{found, point, distance}`: the closest point of the mesh |
| `nav.raycast {from, to, range?}` | `{onMesh, hit, point, normal, distance}`: walks straight on the mesh; `hit` when an edge (wall, drop) stops it. A line of walk, not of sight |
| `render.screenshot {navmesh: true}` | Draws the mesh: light blue borders, darker lines between polygons |

Heights of the returned points follow the baked surface within about a quarter of `cellSize`.

## Lua

```lua
local path, partial = nav.path(from, to)        -- array of {x,y,z}, or nil when an end is off the mesh
local point = nav.nearest(position, range)      -- {x,y,z} or nil
local hit = nav.raycast(from, to)               -- nil when the way is clear, else {point, normal, distance}
nav.bake()                                      -- after the level changed; true when there is a mesh
```

Following a path is script work. The official module `oe.nav` holds it:

```lua
local navigation = require("oe.nav")
local Enemy = {}

function Enemy:onStart()
  self.agent = navigation.agent(self.id, { speed = 3.5 })
  self.player = scene.find("Player")
end

function Enemy:onUpdate(dt)
  self.timer = (self.timer or 0) - dt
  if self.timer <= 0 then                       -- chase: look for the way again twice a second
    self.timer = 0.5
    self.agent:moveTo(scene.get(self.player, "Transform").position)
  end
  self.agent:update(dt)                         -- true while on its way
end

return Enemy
```

| `oe.nav` | |
|---|---|
| `agent(id, {speed, stoppingDistance, turnSpeed, faceOffset, repath})` | An agent for an entity. Defaults: 3.5 m/s, 0.1 m, 720 deg/s (0 keeps the rotation), 0 (`faceOffset` 180 for models that look along +Z), 0.5 s between path refreshes (0 = never) |
| `agent:moveTo(point)` | Finds the path from where the entity is; false when there is none |
| `agent:update(dt)` | Moves one step; true while on its way. With a `CharacterBody` it sets the horizontal velocity (physics keeps walls, steps and gravity); otherwise it moves the `Transform` along the mesh, keeping the entity's height above it |
| `agent:stop()`, `agent:remaining()` | Stop; meters left along the path |
| `agent.moving`, `agent.partial` | On its way; the goal is unreachable and it walks as near as it can |

## Editor

Scene view toolbar **NavMesh** (also View > NavMesh) draws the mesh and follows edits. `NavMesh` and `NavModifier`
are ordinary components in the Inspector.

## Implementation

- `engine/nav/NavSources.cpp` turns the scene into triangles (`CollectNavGeometry`), in entity id order.
- `engine/nav/Navigation.cpp` is the only code that knows Recast and Detour (`third_party/recast`, 1.6.0,
  unmodified): a tiled bake (64-cell tiles, watershed regions) and `dtNavMeshQuery` for the queries.
- `Engine::Nav()` owns the mesh and decides when to bake (hash of the collected geometry and settings).

## Status

Not implemented: avoidance between agents (they walk through each other unless physics separates them), off-mesh
links (jumps, ladders), area costs, moving obstacles that carve the mesh without a full bake, several agent sizes
at once, 2D (XY plane) navigation, a baked mesh stored with the project. Verified on Windows (tests `NavMeshBakeAndQueries`,
`NavAgentWalksPath`) and in the web runtime (headless Edge: same mesh, path and end position). Unverified: the
Android runtime was rebuilt with it but not run; the editor overlay was checked only in headless screenshots.
