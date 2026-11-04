# The framework (the engine's systems and the frame loop) and the applications it runs.
add_library(de_app STATIC
    frame_clock.h
    framework.cpp
    framework.h
    viewer.cpp
    viewer.h)
add_library(de::app ALIAS de_app)

target_link_libraries(de_app
    PUBLIC de::asset de::core de::platform de::rhi de::render_graph de::render_scene de::shader de::tools de::world
           de::compile_options
    PRIVATE de::render_imgui)
# Dev builds mount the source tree as src:/ (shaders compile from it and hot reload).
target_compile_definitions(de_app PRIVATE "DE_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
