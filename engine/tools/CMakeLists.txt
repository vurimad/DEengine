find_package(imgui CONFIG REQUIRED)

# Debug and editor tools drawn with Dear ImGui.
add_library(de_tools STATIC
    debug_ui.cpp
    debug_ui.h
    stats_panel.cpp
    stats_panel.h)
add_library(de::tools ALIAS de_tools)

target_link_libraries(de_tools
    PUBLIC de::core de::rhi de::render_graph de::compile_options imgui::imgui)
