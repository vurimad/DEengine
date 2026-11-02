find_package(imgui CONFIG REQUIRED)

# Dear ImGui's renderer on the RHI and the render graph.
add_library(de_render_imgui STATIC
    imgui_renderer.cpp
    imgui_renderer.h)
add_library(de::render_imgui ALIAS de_render_imgui)

target_link_libraries(de_render_imgui
    PUBLIC de::core de::rhi de::render_graph de::shader de::compile_options
    PRIVATE imgui::imgui)
