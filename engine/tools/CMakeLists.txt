find_package(imgui CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(yyjson CONFIG REQUIRED)

# Debug and editor tools drawn with Dear ImGui, the console window, the agents' socket, and the agent's eyes: camera
# scripts and screenshots.
add_library(de_tools STATIC
    agent_server.cpp
    agent_server.h
    camera_script.cpp
    camera_script.h
    console_ui.cpp
    console_ui.h
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
    PUBLIC de::console de::core de::platform de::rhi de::render_graph de::world de::compile_options imgui::imgui
    PRIVATE de::render_imgui de::shader yyjson::yyjson)
