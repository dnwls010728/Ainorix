# Physics

3D rigid-body physics powered by [Jolt Physics](https://github.com/jrouwe/JoltPhysics) 5.6.0 (vendored in `third_party/jolt`, MIT). Jolt runs in **cross-platform deterministic** mode with a single-threaded job system, so the same scene and the same inputs produce bit-identical results on every machine — frame hashes stay usable as test oracles.

Jolt is hidden behind `engine/physics/PhysicsWorld` — the components, commands and Lua API below are the engine's own and would stay the same if the backend changed (e.g. a Box2D backend for 2D).

Physics runs only while simulating. Each fixed step (1/60 s): **scripts `onUpdate` → built-in systems (PlayerController, …) → physics → collision/trigger callbacks**.

## Components

| Component | Purpose |
|---|---|
| `Collider` | Shape: `box` (`size`, matches the 1×1×1 cube mesh by default), `sphere` (`radius`), `capsule` (`radius`, `height`). `center` offsets it. Scaled by the entity's world scale. `friction`, `bounciness`. Without a RigidBody it is **static** (floors, walls). |
| `Collider` + `isTrigger` | Non-solid volume. Reports `onTriggerEnter/Exit` for moving bodies and characters (not static colliders). |
| `RigidBody` | `dynamic` (gravity, collisions, can be pushed) or `kinematic` (follows its Transform — moving platforms, doors — and pushes dynamic bodies). `mass`, `gravityScale`, `linearDamping`, `lockRotation`, `continuous` (anti-tunneling for fast objects). `velocity` / `angularVelocity` are written every step; set them to launch a body. |
| `CharacterBody` | Player/NPC controller: capsule or sphere centered on the entity. Set `velocity` (x/z to walk, y to jump); the engine slides along walls, climbs `stepHeight` steps and slopes up to `maxSlope`, applies gravity and sets `grounded`. Replaces the Collider on the same entity. |

`PlayerController` automatically drives a `CharacterBody` when the entity has one (otherwise it falls back to its simple y = 0 ground). Things moved by editing their Transform: statics are teleported, kinematics are moved smoothly, dynamics are teleported and keep their velocity.

Example — a crate the player can push, a ball that bounces:

```bash
oe exec my_game entity.create '{"name":"Crate","components":{"Transform":{"position":[0,3,0]},"MeshRenderer":{},"Collider":{},"RigidBody":{"mass":2}}}' --save
oe exec my_game entity.create '{"name":"Ball","components":{"Transform":{"position":[1,4,0]},"MeshRenderer":{"mesh":"sphere"},"Collider":{"shape":"sphere","bounciness":0.7},"RigidBody":{}}}' --save
```

## Script callbacks

```lua
function M:onCollisionEnter(other) end   -- other = entity id
function M:onCollisionExit(other) end
function M:onTriggerEnter(other) end     -- called on the trigger and on the other entity
function M:onTriggerExit(other) end
```

Events are sorted by entity id, so their order is deterministic. Resting objects that fall asleep keep their contact (no spurious exit/enter).

Lua API: `physics.raycast(origin, dir, maxDist?)` → `{entity, point, normal, distance}` or `nil`; `physics.overlapSphere(center, radius)` → ids; `physics.addImpulse(id, v)`; `physics.contacts(id)` → ids touching it. Instance helpers: `self:grounded()`, `self:velocity()`, `self:setVelocity(x, y, z)`, `self:addImpulse(x, y, z)`, `self:contacts()`.

## Commands (for agents)

| Command | Use |
|---|---|
| `physics.raycast {origin, direction, maxDistance?}` | First hit: entity, point, normal, distance. Works outside simulation too |
| `physics.overlap {center, radius}` | Entities intersecting a sphere |
| `physics.contacts {id?}` | Pairs touching after the last step, `collision` or `trigger` — answers "why isn't it colliding?" |
| `physics.state` | Backend, gravity, body counts, warnings (invalid shapes, CharacterBody + Collider on one entity) |
| `render.screenshot {colliders: true}` | Draws collider wireframes (green solid, yellow trigger, cyan character) so shapes can be checked visually |

`oe render <project> --colliders` and the editor's **Colliders** checkbox show the same wireframes.

## Limits (current)

- Gravity is fixed at 9.81 m/s² downward (`gravityScale` per body/character).
- No collision layers/masks or joints yet; no mesh colliders.
- Characters ignore entity scale (their size is `radius`/`height` in meters).
- 2D: planned as a per-scene mode that locks bodies to the XY plane (Jolt `EAllowedDOFs::Plane2D`), with an optional Box2D backend later — see [ROADMAP.md](ROADMAP.md).
