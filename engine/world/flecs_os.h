#pragma once

// Inside world/ only: flecs' OS API, routed through the engine (docs/design.md, World data rules).

namespace de::world::detail {

// flecs' memory on the World heap (counted per frame like all engine memory), its log in ours,
// file access off (files go through the VFS), its own threads off (parallel systems use
// JobSystem::parallel_for). flecs' OS API is process-wide: installed once, before the first world.
void install_flecs_os_api();

// Whether flecs is running on the engine's hooks (for tests).
bool flecs_os_api_installed();

} // namespace de::world::detail
