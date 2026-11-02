add_library(de_render_graph STATIC
    render_graph.cpp
    render_graph.h)
add_library(de::render_graph ALIAS de_render_graph)

target_link_libraries(de_render_graph PUBLIC de::core de::rhi de::compile_options)
