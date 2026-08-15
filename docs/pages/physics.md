# Physics and Spatial Partitioning {#physics}

This page describes the rigid-body physics simulation and the broadphase
spatial structure in SleakEngine.

| Type | Role |
| :--- | :--- |
| `Sleak::Physics::PhysicsWorld` | Owns the collider list and the broadphase tree; stepped once per frame. |
| `Sleak::Physics::DynamicAABBTree` | Self-balancing AABB tree used as the broadphase and for queries. |
| `Sleak::ColliderComponent` | Attaches a shape to a `GameObject` and registers it with the world. |
| `Sleak::RigidbodyComponent` | Velocity, gravity, mass, and collision-normal bookkeeping. |
| `Sleak::Physics::ColliderShape` | `std::variant` over `AABB`, `BoundingSphere`, `BoundingCapsule`, `TriangleMesh`. |
| `Sleak::Physics::CollisionManifold` | Narrow-phase result: contact point, normal, penetration. |
| `Sleak::Physics::RayHit` / `SweepResult` | Query results from `Raycast` and `SphereSweep`. |

\dot
digraph step {
  bgcolor="transparent"; rankdir=TB;
  node [shape=box, style="rounded,filled", fillcolor="#1d4ed822", color="#3b82f6", fontcolor="#7aa7d9", fontname="Helvetica", fontsize=11];
  edge [color="#557799", fontcolor="#557799", fontname="Helvetica", fontsize=10];
  step  [label="PhysicsWorld::Step(dt)", fillcolor="#22d3ee22", color="#22d3ee"];
  integ [label="integrate, inline in Step\nper Dynamic body:\ngravity, terminal clamp,\nTransform::Translate"];
  broad [label="UpdateBroadphase()\nDynamicAABBTree::MoveProxy\nper collider"];
  pairs [label="FindPairsAndResolve()"];
  query [label="DynamicAABBTree::Query\ncandidate pairs"];
  filt  [label="dedupe by pointer order,\nlayer and mask filter"];
  narrow[label="TestCollision(shapeA, shapeB)\n-> CollisionManifold"];
  res   [label="RigidbodyComponent::ResolveCollision\n(normal, penetration)", fillcolor="#22d3ee22", color="#22d3ee"];
  step -> integ -> broad -> pairs;
  pairs -> query -> filt -> narrow -> res;
}
\enddot

One `Step` call integrates, refits the tree, then resolves every overlapping
pair inline. There is no separate solver iteration and no contact cache.

---

## 1. Physics World (`Sleak::Physics::PhysicsWorld`)

`Sleak::Physics::PhysicsWorld` (`include/public/Physics/PhysicsWorld.hpp`)
is stepped once per frame from `SceneBase::Update(deltaTime)`, using the
variable frame delta time. It is not on the engine's fixed-timestep path;
`SceneBase::FixedUpdate` exists and is driven by a real accumulator in
`Application::Run()` (see @ref rendering_pipeline), but `PhysicsWorld` does
not use it.

`PhysicsWorld::Step` integrates gravity and linear velocity into position
for `BodyType::Dynamic` bodies, with grounded-state tracking and a terminal
velocity clamp. There is no angular velocity and no damping anywhere in the
physics code.

There is no collision enter/stay/exit callback system. Collision response is
resolved inline: `PhysicsWorld::FindPairsAndResolve` calls
`RigidbodyComponent::ResolveCollision(normal, penetration)` directly on
overlapping pairs. `ColliderComponent::IsTrigger()` skips physical
resolution between two triggers, but nothing fires an event when that
happens. `PhysicsWorld::Raycast(origin, direction, maxDist, layerMask)`
returns a `RayHit` struct; it is implemented as a small-radius sphere
sweep, not by exposing a raycast callback.

`PhysicsWorld` is a small, general-purpose system; in SleakCraft it is used
narrowly to track the player camera's grounded/collision state. World
collision against voxel geometry is handled by a separate system,
`VoxelQueries::ResolveVoxelCollision`, and does not go through
`PhysicsWorld` at all.

---

## 2. Dynamic AABB Tree (`Sleak::Physics::DynamicAABBTree`)

`Sleak::Physics::DynamicAABBTree`
(`include/public/Physics/DynamicAABBTree.hpp`) is a genuinely self-balancing
tree: `Balance(nodeId)` computes a height-difference balance factor and
performs AVL-style rotations with AABB and height refit after every insert
or remove. Proxy AABBs are fattened by a fixed margin
(`FAT_AABB_MARGIN = 0.1f`) via `AABB::Fatten()`, and `MoveProxy` extends the
fat box toward the direction of travel, skipping re-insertion when the new
AABB is still contained in the old one.

```cpp
void Query(const AABB& queryAABB, const std::function<bool(int)>& callback) const;
void RayCast(Vector3D origin, Vector3D direction, float maxDist,
             const std::function<bool(int)>& callback) const;
```

Both callbacks receive a proxy id (`int`), not a collider pointer, and
return `bool` to continue or stop traversal.

The tree is not used for frustum culling; the @ref culling "culling system"
has no reference to it and is a separate CPU software-occlusion system.
`DynamicAABBTree` usage is confined to `include/public/Physics/` and
`src/Physics/`.

---

## 3. Colliders and Rigidbodies

`Sleak::RigidbodyComponent` (`include/public/Physics/RigidbodyComponent.hpp`)
holds a `BodyType` (`Static`, `Kinematic`, `Dynamic`), `Vector3D m_velocity`,
`Vector3D m_gravity` (default `(0, -9.81, 0)`), `bool m_useGravity`,
`float m_mass` (default `1.0`), `float m_terminalVelocity` (default `50.0`),
and ground/wall collision-normal bookkeeping. There is no friction field, no
restitution field, and no angular velocity field.

`Sleak::ColliderComponent` (`include/public/Physics/ColliderComponent.hpp`)
is a concrete `Component`, not an abstract base with per-shape subclasses.
It holds a `Physics::ColliderShape`, defined as
`std::variant<AABB, BoundingSphere, BoundingCapsule, TriangleMesh>`
(`include/public/Physics/Colliders.hpp`), selected through constructor
overloads or a `ColliderType` enum. There are no `BoxCollider`,
`SphereCollider`, or `CapsuleCollider` classes; the real shape types are
`Physics::AABB`, `Physics::BoundingSphere`, `Physics::BoundingCapsule`
(a genuine cylinder-plus-hemispherical-caps shape, suitable for character
controllers), and `Physics::TriangleMesh`.

`Physics::AABB` is a distinct type from `Sleak::Math::AABB`
(`include/public/Math/AABB.hpp`); the physics variant additionally carries
`Fatten()`, `GetSurfaceArea()`, and `Merge()` for use as the broadphase and
collision shape. The two also spell their accessors differently
(`Physics::AABB::GetCenter()` against `Math::AABB::Center()`), so a mix-up
shows up as a compile error rather than silently wrong math.

---

## 4. Queries

Beyond `Step`, `PhysicsWorld` answers four spatial queries against
everything registered with `RegisterCollider`:

```cpp
std::vector<CollisionPair> OverlapSphere(const Vector3D& center, float radius,
                                          uint32_t layerMask = 0xFFFFFFFF) const;
std::vector<CollisionPair> OverlapAABB(const AABB& aabb,
                                        uint32_t layerMask = 0xFFFFFFFF) const;
SweepResult SphereSweep(const Vector3D& start, const Vector3D& direction,
                         float radius, float maxDist,
                         uint32_t layerMask = 0xFFFFFFFF) const;
RayHit Raycast(const Vector3D& origin, const Vector3D& direction,
                float maxDist, uint32_t layerMask = 0xFFFFFFFF) const;
```

Every query takes a layer mask and tests it against each collider's own
layer, so a query can ignore whole categories of geometry. The overlap
queries return `CollisionPair` values carrying the full manifold, not just
the collider pointer, so a caller can act on the contact normal without
running the narrow phase again.

Colliders register themselves with the scene's world through
`ColliderComponent`, so a query sees an object as soon as its collider
component initializes and stops seeing it once the component is removed.

---

## 5. Where to Look in the Source

| Question | File |
| :--- | :--- |
| The step order and query implementations | `src/Physics/PhysicsWorld.cpp` |
| Tree insertion, removal, and AVL balancing | `src/Physics/DynamicAABBTree.cpp` |
| Shape definitions and the world-AABB helper | `include/public/Physics/Colliders.hpp` |
| Every shape-versus-shape test | `include/public/Physics/CollisionDetection.hpp` |
| How a collision changes velocity | `src/Physics/RigidbodyComponent.cpp` |
| Where the world gets stepped | `src/Scene/SceneBase.cpp` |
