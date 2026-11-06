# physics

Jolt on the engine's memory and jobs, stepping with the world (docs/design.md, Physics, camera, input & audio).

| File | What it gives |
| --- | --- |
| `physics.h` | `Physics`, and the components: `Collider` and `RigidBody` for bodies, `CharacterController` for what walks |
| `jolt_shapes.h` | What the cooker shares with it, with no Jolt types: Jolt's setup, and building a mesh shape's bytes (library `de::physics_shapes`) |

## How it fits

- **Create `Physics` before the world and call `add_to_world` once the world exists.** It steps in `Phase::Physics`, at the world's fixed tick. The world's last entities let go of their bodies as it goes down, so `Physics` has to outlive it.
- **Components drive it.** `Collider` and `RigidBody` on an entity give it a body at the start of the next tick, from its world transform, so `place()` it first. Taking either away takes the body away. Changing either later doesn't change the body.
- **Static bodies cost nothing per tick.** Kinematic ones head for their `Transform`, and a dynamic one writes its `Transform` while it's awake. `place()` on a dynamic body or a character teleports it. A static one stays where it was made.
- **Characters aren't bodies.** A `CharacterController` is a Jolt `CharacterVirtual`, with its entity's position at its feet. Set `desired_velocity_mps` across the ground, which is where root motion goes. Rays and shape casts don't hit them, and combat doesn't use them: it has its own swept capsules.
- **Jolt's types stay inside this library.** Positions are doubles at both ends.

## Static collision from meshes

A cell entity with a mesh asks for it with `"collision": "mesh"` (`tools/cooker/cell_cooker.h`). From code, it's a `world::MeshCollider` on an entity, naming a `.dephys` the caller holds, with `PhysicsDesc::assets` set.

- **What's cooked.** Every model cooks a `.dephys` beside its `.demesh`: a Jolt `MeshShape`'s binary state, from the finest LOD with at most `collision_triangles` triangles (the `mesh` setting, 2048 by default), with every position welded to one vertex. `tools/cooker/README.md` says why.
- **One shape, however many bodies.** All the bodies of one asset share one shape, restored the first time one's needed: its file is checked first (`verify_content_hash`, then `mesh_shape_format()`), as Jolt believes what it reads. A scale other than 1 is a `JPH::ScaledShape` around that shape, shared by what has exactly that scale. `PhysicsStats::shapes` counts them both.
- **Holding.** A cell has its entities' `.dephys` as dependencies, so it isn't Ready, and its members don't exist, until they're loaded, and they go after its bodies. A `MeshCollider` set from code doesn't hold its asset, like `MeshInstance`: whoever set it does. Its body waits, a look at the registry each tick, and the frame doesn't. Physics keeps a shape while the registry has its asset: on `Unloaded` the shape goes.
- **A reload** swaps the shape: the bodies that use it go, and come back in the next tick. A refused asset that's cooked again, or a failed one whose file appears, gets its bodies then too.
- **Refused:** a failed or corrupt asset, one from another Jolt, a scale with a zero. Logged, no body, and not asked about again. The rest of the world carries on.

## What meshes cost

`benchmark: physics at scale` in `tests/physics/physics_test.cpp` with `DE_PHYSICS_BENCH=1`, from a dev build, on a boulder of 1.3 x 1.0 x 1.8 m, on a machine doing other things: read these as rough.

| On 10,000 of them | A box of its bounds | Mesh, 1,032 triangles (the default) | Mesh, 4,132 | Mesh, 66,122 (all of it) |
| --- | --- | --- | --- | --- |
| A ray straight down, one thread | 300 ns | 420-520 ns | 460-580 ns | 500-600 ns |
| A ray across the field | 400 ns | 870-1130 ns | 1000-1300 ns | 1150-1400 ns |
| 200 characters on them, with what fell | 2.5 ms a tick | 11 ms | 28 ms | 430 ms |
| Size of one shape | a few hundred bytes | 16 KB | 64 KB | 1 MB |

A character's capsule is about as big as this boulder, so it meets most of its triangles: triangles are what characters pay for, far more than rays, whose tree makes them cheap. That's what the limit on triangles is for. Spheres cost about the same falling onto meshes as onto boxes: 10,000 settling take 4.2 ms a tick where 4,700 of them are still awake, against 0.8 ms where 590 are, because the boxes' flat tops let them sleep and the boulders roll them off. A thousand instances add 252 bytes each. Restoring and adding the first body took a tick of 50-120 us, against 3 us for an empty one.

## Build

`cmake/vcpkg-ports/joltphysics` is vcpkg's port with `DOUBLE_PRECISION` and `CROSS_PLATFORM_DETERMINISTIC` on, because the design doc asks for doubles and strict determinism and both change Jolt's types and code. It's built as a static library (`cmake/vcpkg-triplets/port-options.cmake`).

## Limits

- **Moving bodies allocate.** Jolt's broad phase takes one small scratch block per tick while anything is moving (`QuadTree::UpdatePrepare`), through `MemoryTag::Physics`, and whatever the body count. It stops when everything sleeps. Static bodies and characters allocate nothing per tick.
- **Characters run on the game thread, one after another,** before the step. Fine for a handful; it hasn't been measured with many enemies.
- **Moving a static body does nothing,** by design: there's no per-tick look at them.
- **Moving bodies must be roots.** Only their own `Transform` is read and written.
- **Meshes are one-sided and static.** That's Jolt's rule for what simulates: a body meeting a triangle from behind goes through it (a character doesn't, and rays and shape casts see what they ask for). Jolt can't collide a dynamic mesh with a static one, so nothing here tries. There's one friction and no restitution per body, and none per triangle.
- **Non-uniform scale works, with one soft edge.** Which edges are smooth (Jolt's active edges, under 5 degrees between triangles) was decided on the unscaled mesh at cook time. Stretching a mesh hard changes those angles without changing that, so what slides along a once-flat crease can catch or dig in a little. A mirror (one or three negative axes) is fine: Jolt flips the winding. A zero in a scale is refused.
- **A shape is in memory twice.** The restored shape is a copy, and the asset's file stays loaded behind it for as long as a cell holds it, about the same size again. Jolt offers no way to use the bytes where they lie.
- **Every model cooks collision,** asked for or not: a few tens of kilobytes on disk, none in memory until something loads it. Nothing yet leaves out what no cell uses.
- **Restoring and making bodies happens on the tick's thread,** in `create_bodies`, not on a job as Open world & streaming means to have it in the end. It's a copy of the shape's bytes and a hash of them, too small to matter at these sizes, but a world loading many large meshes at once would want it on a job.

## Not here yet

Heightfield terrain; a debug view of shapes; soft bodies for cloth; ragdolls.
