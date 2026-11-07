find_package(imgui CONFIG REQUIRED)
find_package(Stb REQUIRED)

# Debug and editor tools drawn with Dear ImGui, and the agent's eyes: camera scripts and screenshots.
add_library(de_tools STATIC
    camera_script.cpp
    camera_script.h
    debug_ui.cpp
    debug_ui.h
    free_camera.cpp
    free_camera.h
    screenshot.cpp
    screenshot.h
    stats_panel.cpp
    stats_panel.h)
add_library(de::tools ALIAS de_tools)

target_include_directories(de_tools SYSTEM PRIVATE "${Stb_INCLUDE_DIR}")

target_link_libraries(de_tools
    PUBLIC de::core de::platform de::rhi de::render_graph de::world de::compile_options imgui::imgui
    PRIVATE de::render_imgui de::shader)
