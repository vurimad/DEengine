#pragma once

// Inside world/ only: flecs' OS API, routed through the engine (docs/design.md, World data rules).

namespace de::core {
class JobSystem;
}

namespace de::world::detail {

// flecs' memory on the World heap (counted per frame like all engine memory), its log in ours,
// file access off (files go through the VFS), its task threads on our JobSystem. flecs' OS API is
// process-wide: installed once, before the first world.
void install_flecs_os_api();

// Where task threads run: the JobSystem of the world that has task threads (one at a time), or null.
void set_flecs_jobs(core::JobSystem* jobs);

} // namespace de::world::detail
