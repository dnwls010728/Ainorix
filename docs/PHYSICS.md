# Physics

3D rigid-body physics powered by [Jolt Physics](https://github.com/jrouwe/JoltPhysics) 5.6.0 (vendored in `third_party/jolt`, MIT) and 2D physics powered by [Box2D](https://box2d.org) 3.1.1 (vendored in `third_party/box2d`, MIT). Both run in **cross-platform deterministic** mode, single-threaded, so the same scene and the same inputs produce bit-identical results on every machine — frame hashes stay usable as test oracles.

The backends are hidden behind `engine/physics/PhysicsWorld` (Jolt) and `engine/physics/Physics2D` (Box2D): the components, commands and Lua API below are the engine's own. A world is created only when the scene has bodies for it; events and queries merge both (2D and 3D bodies do not collide with each other; tilemaps collide with both).

Physics runs only while simulating. Each fixed step (1/60 s): **scripts `onUpdate` → built-in systems (Animator, ParticleEmitter, SpriteAnimation) → physics → collision/trigger callbacks**.

## Components

| Component | Purpose |
|---|---|
| `Collider` | Shape: `box` (`size`, matches the 1×1×1 cube mesh by default), `sphere` (`radius`), `capsule` (`radius`, `height`). `center` offsets it. Scaled by the entity's world scale. `friction`, `bounciness`. Without a RigidBody it is **static** (floors, walls). |
| `Collider` + `isTrigger` | Non-solid volume. Reports `onTriggerEnter/Exit` for moving bodies and characters (not static colliders). |
| `RigidBody` | `dynamic` (gravity, collisions, can be pushed) or `kinematic` (follows its Transform — moving platforms, doors — and pushes dynamic bodies). `mass`, `gravityScale`, `linearDamping`, `continuous` (anti-tunneling for fast objects), axis locks (below). `velocity` / `angularVelocity` are written every step; set them to launch a body. |
| `CharacterBody` | Player/NPC controller: capsule or sphere centered on the entity. Set `velocity` (x/z to walk, y to jump); the engine slides along walls, climbs `stepHeight` steps (also steps lower than the capsule's radius, which its round bottom meets at their edge) and slopes up to `maxSlope`, applies gravity and sets `grounded`. Replaces the Collider on the same entity. `lockPositionX/Y/Z` keep it on its X, height or Z (`lockPositionZ` for 2.5D side-scrollers; `lockPositionY` also turns gravity off). Hitting a ceiling ends upward motion. |

**Axis locks** (Unity's Rigidbody constraints, Godot's `axis_lock_*`): `RigidBody.lockPositionX/Y/Z` stop a dynamic body moving along a world axis and `lockRotationX/Y/Z` stop it rotating around one (all three keep it upright; `lockPositionZ` + `lockRotationX/Y` keep a 2.5D body in its XY plane). Jolt removes those degrees of freedom; a body with all six locked is held like a kinematic body. `RigidBody2D.lockPositionX/Y` and `lockRotationZ` do the same in 2D (Box2D 3.1 has no axis locks: a shapeless static anchor and a wheel or revolute joint hold the body origin, about 0.0001 m of give under gravity; moving the body by its Transform moves the lock). The inspector shows each group as one row of X/Y/Z toggles. Older fields still load and are renamed: `RigidBody.lockRotation` (all three rotation locks), `RigidBody.plane2D` (`lockPositionZ` + `lockRotationX/Y`), `CharacterBody.plane2D` (`lockPositionZ`), `RigidBody2D.fixedRotation` (`lockRotationZ`).

A `Tilemap` with colliding tiles becomes one static body made of merged boxes (and prisms for slopes, see [2D.md](2D.md)).

The template's `scripts/player_controller.lua` sets the `CharacterBody` velocity when the entity has one (otherwise it moves the Transform). Things moved by editing their Transform: statics are teleported, kinematics are moved smoothly, dynamics are teleported and keep their velocity. A query between two steps (a script's `physics.raycast`, particles colliding with the world) sees a moved kinematic body at its new Transform, and the next step still sweeps it there from where the last step left it, so a platform keeps pushing and carrying.

Example — a crate the player can push, a ball that bounces:

```bash
oe exec my_game entity.create '{"name":"Crate","components":{"Transform":{"position":[0,3,0]},"MeshRenderer":{},"Collider":{},"RigidBody":{"mass":2}}}' --save
oe exec my_game entity.create '{"name":"Ball","components":{"Transform":{"position":[1,4,0]},"MeshRenderer":{"mesh":"sphere"},"Collider":{"shape":"sphere","bounciness":0.7},"RigidBody":{}}}' --save
```

## Joints

`Joint` (3D, Jolt) and `Joint2D` (Box2D) connect the body of their entity to another body, or to the world when
`connected` is 0. The entity needs a body of its own (`Collider` + `RigidBody`, or the 2D pair); the connected
entity needs at least a collider; one of the two must be dynamic. The bodies are joined **as they stand** when the
joint is made: place them first, then add the joint (the hinge angle, slider position and rod length start from that
pose).

| type | 3D | 2D | What it leaves free |
|---|---|---|---|
| `fixed` | yes | yes | Nothing: glued parts, something that breaks off (`breakForce`). 2D: `frequency` above 0 lets the weld flex. |
| `hinge` | yes | yes | Turning around `axis` through `anchor` (2D: around `anchor`): doors, wheels, levers, swings, flippers. Limits in degrees, motor in degrees/s. |
| `ball` | yes | - | Turning any way around `anchor`: rope and chain links, pendulums, ragdoll shoulders. `useLimits` + `max` keeps it inside a cone around `axis`. |
| `slider` | yes | yes | Moving along `axis`, no turning: lifts, pistons, drawers. Limits in meters, motor in m/s. |
| `distance` | yes | yes | Everything except the distance between `anchor` and `connectedAnchor`: a rod; with `useLimits` a rope (`min` 0, `max` its length); with `frequency` a spring. |
| `wheel` | - | yes | Sliding along `axis` on a spring (`frequency`, `damping`) and spinning: a car wheel with suspension, motor in degrees/s. |

- **Anchors**: `anchor` is a point on this entity in its local units (like `Collider.center`). With `autoAnchor`
  (default) the other body is held at the same world point. For a `distance` joint, or to pull two bodies together,
  set `autoAnchor: false` and `connectedAnchor` (local to the connected entity; world coordinates when `connected`
  is 0). `axis` is in this entity's local axes.
- **Limits** (`useLimits`, `min`, `max`) are measured from the starting pose: hinge angles (3D: `min` in -180..0 and
  `max` in 0..180), slider meters, distance lengths. **Motors** (`useMotor`, `motorSpeed`, `maxMotorForce`) drive a
  hinge, slider or 2D wheel; they may change every frame from Lua without the joint being made again
  (`self:set("Joint", {motorSpeed = 180})`). A motor that cannot move its load needs a larger `maxMotorForce`.
- **Springs**: `frequency` (Hz) and `damping` (ratio, 1 = no overshoot). 3D: the distance rod, and the limits of a
  hinge or slider. 2D: the distance rod, the wheel suspension, the fixed weld.
- **Collision**: the two jointed bodies do not collide with each other unless `collideConnected`.
- **Breaking**: with `breakForce` above 0 the joint comes apart when it had to hold more than that many newtons in a
  step; `broken` becomes true (read it from Lua) and setting it false joins the bodies again where they are.
  `physics.joints` reports the `force` each joint held in the last step, which is how to pick a threshold.
- One joint component per entity. A body with two joints carries one of them; the other body carries the second
  (a chain: every link holds the joint to the link before it).
- Changing any other field, or replacing either body, makes the joint again from the current poses. A body that is
  removed takes its joints with it. `CharacterBody`, `CharacterBody2D` and triggers cannot be jointed.
- `physics.joints {id?}` lists `{id, name, component, type, connected, connectedName, active, broken, force}`;
  `active: false` means a body is missing (the reason is in `physics.state` warnings) or the joint broke.
  `render.screenshot {colliders: true}` draws a magenta cross at each anchor, the hinge or slider axis and the line
  to a separate connected anchor.
- Joints are part of rollback and `sim.save_state` snapshots.

```json
{"command": "entity.create", "args": {"name": "Door", "components": {
  "Transform": {"position": [0, 1, 0]}, "MeshRenderer": {"mesh": "cube"},
  "Collider": {"size": [1, 2, 0.1]}, "RigidBody": {},
  "Joint": {"type": "hinge", "anchor": [-0.5, 0, 0], "axis": [0, 1, 0], "useLimits": true, "min": -90, "max": 90}}}}
```

## 2D physics (Box2D)

For 2D games use the 2D components: shapes are exact in the plane (no Z depth), and Box2D brings one-way platforms, polygons, smooth tile outlines and collision layers. Bodies move in their entity's XY plane: position x/y and rotation around Z are simulated, z and the X/Y rotation are kept; `Transform.scale` x/y scales shapes.

| Component | Purpose |
|---|---|
| `Collider2D` | `shape`: `box` (`size` x/y, `rounding` for rounded corners), `circle` (`radius`), `capsule` (`radius`, `height`, vertical), `polygon` (`points` [[x,y], ...]: any simple outline — concave ones are split into convex pieces), `edge` (`points`: a two-sided line strip; `loop: true` closes it into a smooth one-sided outline, solid inside). `center`, `angle` offset it. `friction`, `bounciness`, `density` (relative, when a body has several shapes). Static without a RigidBody2D. |
| `Collider2D.framePoints` | Polygon outline per sprite frame, `{"<frame>": [[x,y], ...]}` (sheet index as text or atlas frame name): while the entity's `Sprite` shows a listed frame the shape is that outline, other frames use `points`. A new outline replaces the shapes on the same body (velocity, joints and trigger state stay). Fit them with `sprite.outline {allFrames: true}` or draw them in the Sprite Editor ([2D.md](2D.md#sprite-editor)). |
| `Collider2D` + `isTrigger` | Sensor: `onTriggerEnter/Exit` for dynamic/kinematic bodies and characters (not static colliders). With a RigidBody2D the sensor moves with the body. |
| `Collider2D` + `oneWay` | Platform that is solid only from above: bodies and characters pass through from below and the sides. |
| `Collider2D.layer` / `ignoreLayers` | Collision layers 0-15. Two shapes collide unless either ignores the other's layer. Rays and overlaps see every layer. |
| `RigidBody2D` | `dynamic` or `kinematic` (follows its Transform: moving platforms carry characters and push bodies). `mass`, `gravityScale` (0 for top-down), `linearDamping`, `angularDamping`, `bullet`, axis locks `lockPositionX/Y` and `lockRotationZ` (continuous collision against other moving bodies; static geometry is always continuous). `velocity` (x, y) and `angularVelocity` (degrees/s) are written every step; set them to launch a body. |
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
- Collision layers exist in 2D only; no mesh colliders. Joints: no gears, pulleys or ragdoll (swing-twist) joints yet, 3D joints have no drive to a target angle or position, and a joint cannot hold a `CharacterBody`.
- Characters ignore entity scale (their size is `radius`/`height` in meters).
- 2D and 3D bodies live in separate worlds and never collide with each other.

## Static VFX particle queries

VFX emitter collision synchronizes once per fixed-step batch and sweeps particle centers against
static non-sensor Jolt bodies or Box2D shapes (selected by the captured emitter dimension).
Dynamic/kinematic bodies are excluded; queries do not advance physics. See
[PARTICLE_COLLISION.md](PARTICLE_COLLISION.md) for modes, timing and point-query limitations.
