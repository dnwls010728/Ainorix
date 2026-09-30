# Physics

3D rigid-body physics powered by [Jolt Physics](https://github.com/jrouwe/JoltPhysics) 5.6.0 (vendored in `third_party/jolt`, MIT) and 2D physics powered by [Box2D](https://box2d.org) 3.1.1 (vendored in `third_party/box2d`, MIT). Both run in **cross-platform deterministic** mode, single-threaded, so the same scene and the same inputs produce bit-identical results on every machine — frame hashes stay usable as test oracles.

The backends are hidden behind `engine/physics/PhysicsWorld` (Jolt) and `engine/physics/Physics2D` (Box2D): the components, commands and Lua API below are the engine's own. A world is created only when the scene has bodies for it; events and queries merge both (2D and 3D bodies do not collide with each other; tilemaps collide with both).

Physics runs only while simulating. Each fixed step (1/60 s): **scripts `onUpdate` → built-in systems (PlayerController, …) → physics → collision/trigger callbacks**.

## Components

| Component | Purpose |
|---|---|
| `Collider` | Shape: `box` (`size`, matches the 1×1×1 cube mesh by default), `sphere` (`radius`), `capsule` (`radius`, `height`). `center` offsets it. Scaled by the entity's world scale. `friction`, `bounciness`. Without a RigidBody it is **static** (floors, walls). |
| `Collider` + `isTrigger` | Non-solid volume. Reports `onTriggerEnter/Exit` for moving bodies and characters (not static colliders). |
| `RigidBody` | `dynamic` (gravity, collisions, can be pushed) or `kinematic` (follows its Transform — moving platforms, doors — and pushes dynamic bodies). `mass`, `gravityScale`, `linearDamping`, `lockRotation`, `continuous` (anti-tunneling for fast objects). `velocity` / `angularVelocity` are written every step; set them to launch a body. |
| `CharacterBody` | Player/NPC controller: capsule or sphere centered on the entity. Set `velocity` (x/z to walk, y to jump); the engine slides along walls, climbs `stepHeight` steps and slopes up to `maxSlope`, applies gravity and sets `grounded`. Replaces the Collider on the same entity. `plane2D` keeps it on its starting Z for 2D games. Hitting a ceiling ends upward motion. |

`RigidBody.plane2D` restricts a body to the XY plane (rotation around Z only). A `Tilemap` with colliding tiles becomes one static body made of merged boxes (and prisms for slopes, see [2D.md](2D.md)).

`PlayerController` automatically drives a `CharacterBody` when the entity has one (otherwise it falls back to its simple y = 0 ground). Things moved by editing their Transform: statics are teleported, kinematics are moved smoothly, dynamics are teleported and keep their velocity.

Example — a crate the player can push, a ball that bounces:

```bash
oe exec my_game entity.create '{"name":"Crate","components":{"Transform":{"position":[0,3,0]},"MeshRenderer":{},"Collider":{},"RigidBody":{"mass":2}}}' --save
oe exec my_game entity.create '{"name":"Ball","components":{"Transform":{"position":[1,4,0]},"MeshRenderer":{"mesh":"sphere"},"Collider":{"shape":"sphere","bounciness":0.7},"RigidBody":{}}}' --save
```

## 2D physics (Box2D)

For 2D games use the 2D components: shapes are exact in the plane (no Z depth), and Box2D brings one-way platforms, polygons, smooth tile outlines and collision layers. Bodies move in their entity's XY plane: position x/y and rotation around Z are simulated, z and the X/Y rotation are kept; `Transform.scale` x/y scales shapes.

| Component | Purpose |
|---|---|
| `Collider2D` | `shape`: `box` (`size` x/y, `rounding` for rounded corners), `circle` (`radius`), `capsule` (`radius`, `height`, vertical), `polygon` (`points` [[x,y], ...]: any simple outline — concave ones are split into convex pieces), `edge` (`points`: a two-sided line strip; `loop: true` closes it into a smooth one-sided outline, solid inside). `center`, `angle` offset it. `friction`, `bounciness`, `density` (relative, when a body has several shapes). Static without a RigidBody2D. |
| `Collider2D` + `isTrigger` | Sensor: `onTriggerEnter/Exit` for dynamic/kinematic bodies and characters (not static colliders). With a RigidBody2D the sensor moves with the body. |
| `Collider2D` + `oneWay` | Platform that is solid only from above: bodies and characters pass through from below and the sides. |
| `Collider2D.layer` / `ignoreLayers` | Collision layers 0-15. Two shapes collide unless either ignores the other's layer. Rays and overlaps see every layer. |
| `RigidBody2D` | `dynamic` or `kinematic` (follows its Transform: moving platforms carry characters and push bodies). `mass`, `gravityScale` (0 for top-down), `linearDamping`, `angularDamping`, `fixedRotation`, `bullet` (continuous collision against other moving bodies; static geometry is always continuous). `velocity` (x, y) and `angularVelocity` (degrees/s) are written every step; set them to launch a body. |
| `CharacterBody2D` | Box2D mover (capsule or circle): set `velocity` every frame. `mode: "platformer"` applies gravity (`gravityScale`) and reports `grounded`; `"topdown"` moves freely. Slides along walls, walks slopes up to `maxSlope` at the requested horizontal speed (never launched off ramps), rides moving platforms, lands on one-way platforms (`dropThrough: true` falls through), pushes dynamic bodies with a force limited by `pushStrength` and softly pushes other characters. `onWall`, `onCeiling` report this step. Other bodies, triggers and rays see it; it ignores entity scale. |

Pitfalls: a Collider2D with a RigidBody (3D) does not move — use RigidBody2D; a CharacterBody2D ignores a Collider2D on the same entity; `physics.state` lists these as warnings. Its `world2D` part has the Box2D body, shape and contact counts.

## Script callbacks

```lua
function M:onCollisionEnter(other) end   -- other = entity id
function M:onCollisionExit(other) end
function M:onTriggerEnter(other) end     -- called on the trigger and on the other entity
function M:onTriggerExit(other) end
```

Events are sorted by entity id, so their order is deterministic. Resting objects that fall asleep keep their contact (no spurious exit/enter).

Lua API: `physics.raycast(origin, dir, maxDist?)` → `{entity, point, normal, distance}` or `nil` (the nearest hit of both worlds; 2D shapes are hit by the ray's XY projection and the hit keeps the ray's z); `physics.overlapSphere(center, radius)` → ids (a circle in 2D); `physics.addImpulse(id, v)`; `physics.contacts(id)` → ids touching it. Instance helpers work with CharacterBody/CharacterBody2D and RigidBody/RigidBody2D: `self:grounded()`, `self:velocity()`, `self:setVelocity(x, y, z)`, `self:addImpulse(x, y, z)`, `self:contacts()`.

## Commands (for agents)

| Command | Use |
|---|---|
| `physics.raycast {origin, direction, maxDistance?}` | First hit: entity, point, normal, distance. Works outside simulation too |
| `physics.overlap {center, radius}` | Entities intersecting a sphere |
| `physics.contacts {id?}` | Pairs touching after the last step, `collision` or `trigger` — answers "why isn't it colliding?" |
| `physics.state` | Backend, gravity, body counts, warnings (invalid shapes, CharacterBody + Collider on one entity) |
| `render.screenshot {colliders: true}` | Draws collider wireframes (green solid, yellow trigger, cyan character, orange one-way) so shapes can be checked visually |

`oe render <project> --colliders` and the editor's **Colliders** checkbox show the same wireframes.

## Limits (current)

- Gravity is fixed at 9.81 m/s² downward (`gravityScale` per body/character).
- Collision layers exist in 2D only; no joints yet; no mesh colliders.
- Characters ignore entity scale (their size is `radius`/`height` in meters).
- 2D and 3D bodies live in separate worlds and never collide with each other.
