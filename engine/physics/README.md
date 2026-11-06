# physics

Jolt on the engine's memory and jobs, stepping with the world (docs/design.md, Physics, camera, input & audio).

| File | What it gives |
| --- | --- |
| `physics.h` | `Physics`, and the components: `Collider` and `RigidBody` for bodies, `CharacterController` for what walks |

## How it fits

- **Create `Physics` before the world and call `add_to_world` once the world exists.** It steps in `Phase::Physics`, at the world's fixed tick. The world's last entities let go of their bodies as it goes down, so `Physics` has to outlive it.
- **Components drive it.** `Collider` and `RigidBody` on an entity give it a body at the start of the next tick, from its world transform, so `place()` it first. Taking either away takes the body away. Changing either later doesn't change the body.
- **Static bodies cost nothing per tick.** Kinematic ones head for their `Transform`, and a dynamic one writes its `Transform` while it's awake. `place()` on a dynamic body or a character teleports it. A static one stays where it was made.
- **Characters aren't bodies.** A `CharacterController` is a Jolt `CharacterVirtual`, with its entity's position at its feet. Set `desired_velocity_mps` across the ground, which is where root motion goes. Rays and shape casts don't hit them, and combat doesn't use them: it has its own swept capsules.
- **Jolt's types stay inside this library.** Positions are doubles at both ends.

## Build

`cmake/vcpkg-ports/joltphysics` is vcpkg's port with `DOUBLE_PRECISION` and `CROSS_PLATFORM_DETERMINISTIC` on, because the design doc asks for doubles and strict determinism and both change Jolt's types and code. It's built as a static library (`cmake/vcpkg-triplets/port-options.cmake`).

## Limits

- **Moving bodies allocate.** Jolt's broad phase takes one small scratch block per tick while anything is moving (`QuadTree::UpdatePrepare`), through `MemoryTag::Physics`, and whatever the body count. It stops when everything sleeps. Static bodies and characters allocate nothing per tick.
- **Characters run on the game thread, one after another,** before the step. Fine for a handful; it hasn't been measured with many enemies.
- **Moving a static body does nothing,** by design: there's no per-tick look at them.
- **Moving bodies must be roots.** Only their own `Transform` is read and written.

## Not here yet

Cooked `.dephys` collision from meshes, and bodies from cells; heightfield terrain; a debug view of shapes; soft bodies for cloth; ragdolls.
