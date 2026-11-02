#pragma once

namespace de::tools {

// The Dear ImGui context for debug and editor UI. Its memory comes from the Tools heap (tagged,
// counted per frame like everything else); docking is on; nothing is saved to an ini file (files
// go through the VFS only). One at a time.
void create_imgui();
void destroy_imgui();

} // namespace de::tools
