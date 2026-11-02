#pragma once

namespace de::platform {

class Window;

// Dear ImGui's platform side: the display, the time step and the inputs it sees, fed from the OS
// here so SDL stays in platform/. Debug and editor UI only (docs/design.md: game UI is RmlUi).
//
//     ImGui::CreateContext();                 // tools::create_imgui() does this, tagged memory
//     platform::imgui_platform_init(window);
//     each frame: pump_events(), then imgui_platform_new_frame(window, dt), then ImGui::NewFrame()
//     platform::imgui_platform_shutdown();    // before the context goes
//
// While it is initialized, pump_events() also hands every mouse, key, text and focus event to
// the current ImGui context. Game input still sees them all; ImGuiIO::WantCaptureMouse and
// WantCaptureKeyboard say when the UI is using them.
void imgui_platform_init(Window& window);
void imgui_platform_shutdown();

// Before ImGui::NewFrame(): the display size in points with its pixel scale, the time step, and
// text input turned on while a text field is active.
void imgui_platform_new_frame(Window& window, float delta_s);

} // namespace de::platform
