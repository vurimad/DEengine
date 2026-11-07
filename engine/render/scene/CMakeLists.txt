# The scene on the GPU (docs/design.md, Rendering): GPU copies of loaded assets, and the pass that
# draws what the world extracts each frame.
add_library(de_render_scene STATIC
    fog_noise.cpp
    fog_noise.h
    gpu_assets.cpp
    gpu_assets.h
    scene_renderer.cpp
    scene_renderer.h
    terrain.cpp
    terrain.h)
add_library(de::render_scene ALIAS de_render_scene)

target_link_libraries(de_render_scene
    PUBLIC de::asset de::core de::rhi de::render_graph de::shader de::compile_options
    PRIVATE de::world)
