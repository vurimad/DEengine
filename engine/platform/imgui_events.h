#pragma once

// Inside platform/ only: pump_events() hands each SDL event to Dear ImGui through this.

union SDL_Event; // NOLINT(readability-identifier-naming): SDL's name

namespace de::platform {

// Does nothing unless imgui_platform_init() ran.
void imgui_platform_event(const SDL_Event& event);

} // namespace de::platform
