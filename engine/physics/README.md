# physics

Jolt on the engine's memory and jobs, stepping with the world (docs/design.md, Physics, camera, input & audio).

| File | What it gives |
| --- | --- |
| `physics.h` | `Physics`, and the components: `Collider` and `RigidBody` for bodies, `CharacterController` for what walks |

## How it fits

- **Create `Physics` before the world and call `add_to_world` once the world exists.** It steps in `Phase::Physics`, at the world's fixed tick. The world's last entities let go of their bodies as it goes down, so `Physics` has to outlive it.
- **Components drive it.** `Collider` and `RigidBody` on an entity give it a body at the start of the next tick, from where its `Transform` says. Taking either away takes the body away. Changing either later doesn't change the body.
- **What's asleep costs nothing per tick.** Static bodies never do. Kinematic ones head for their `Transform`, and a dynamic one writes its `Transform` while it's awake and in the step it falls asleep. Writing a `Transform` from elsewhere teleports a dynamic body or a character, by a difference of more than a micrometer, and has every row of its table looked at once. A static one stays where it was made.
- **Characters aren't bodies.** A `CharacterController` is a Jolt `CharacterVirtual`, with its entity's position at its feet. Set `desired_velocity_mps` across the ground, which is where root motion goes. Rays and shape casts don't hit them, and combat doesn't use them: it has its own swept capsules.
- **Jolt's types stay inside this library.** Positions are doubles at both ends.

## Build

`cmake/vcpkg-ports/joltphysics` is vcpkg's port with `DOUBLE_PRECISION` and `CROSS_PLATFORM_DETERMINISTIC` on, because the design doc asks for doubles and strict determinism and both change Jolt's types and code. It's built as a static library (`cmake/vcpkg-triplets/port-options.cmake`).

It needs AVX2, on Windows and Linux: that's Jolt's default, and the port names the platform on Windows, where vcpkg's Ninja build would otherwise leave Jolt on SSE2 without a word. `CROSS_PLATFORM_DETERMINISTIC` keeps fused multiply-add off.

That goes for the whole program, not this library. The flag is on Jolt and `physics.cpp` only, but Jolt's static initializers run before `main`, and the linker keeps AVX-encoded copies of inline functions shared with the rest (`glm`, `flecs`, `std`, even `core::align_up`). So whatever uses `Physics` gets the check in `platform/cpu.h`, before any static initializer.

Under `linux-clang`, `cmake/vcpkg-triplets/x64-linux-clang.cmake` builds Jolt with clang. vcpkg would use GCC: with that, Jolt's `IsNormalized` assert fired on the first call to `SetPositionAndRotation`, and with clang it doesn't. Jolt's API passes vectors by value, so the compilers disagreeing about how is the likely reason, though nobody has read how each passes them.

## Limits

- **Moving bodies allocate.** Jolt's broad phase takes one small scratch block per tick while anything is moving (`QuadTree::UpdatePrepare`), through `MemoryTag::Physics`, and whatever the body count. It stops when everything sleeps. Static bodies and characters allocate nothing per tick.
- **Characters run on the game thread, one after another,** before the step. Fine for a handful; it hasn't been measured with many enemies.
- **Moving a static body does nothing,** by design: there's no per-tick look at them.
- **What writes a `Transform` is still per table.** Kinematic bodies and characters do, so one of them has its whole table copied and looked through by the renderer, and a `place()` has every row of its table compared once. They shouldn't share a table with thousands that don't move. Dynamic bodies don't have this: they're written and reported a row at a time, so a prop waking among thousands costs what the prop does.
- **Moving bodies and characters must be roots.** They're refused with an error otherwise, when they're made or when `world::attach()` gives them a parent later, and they lose what they had. Giving them their `RigidBody` or `CharacterController` again asks again.

## Not here yet

Cooked `.dephys` collision from meshes, and bodies from cells; heightfield terrain; a debug view of shapes; soft bodies for cloth; ragdolls.
