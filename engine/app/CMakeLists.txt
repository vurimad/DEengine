# The framework (the engine's systems and the frame loop) and the applications it runs.
add_library(de_app STATIC
    agent_eyes.cpp
    agent_eyes.h
    frame_clock.h
    framework.cpp
    framework.h
    viewer.cpp
    viewer.h)
add_library(de::app ALIAS de_app)

target_link_libraries(de_app
    PUBLIC de::asset de::core de::physics de::platform de::rhi de::render_graph de::render_scene de::shader de::tools de::world
           de::compile_options
    PRIVATE de::render_imgui)
# Dev builds mount the engine's sources as engine:/ (shaders compile from it and hot reload) and a project's as src:/:
# the one --project names, else DE_DEFAULT_PROJECT (CMakeLists.txt).
target_compile_definitions(de_app PRIVATE "DE_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
    "DE_DEFAULT_PROJECT=\"${DE_DEFAULT_PROJECT}\"")
