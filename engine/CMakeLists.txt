add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(render)

# The one engine executable: game, editor (--editor) and headless (--headless) are modes of it.
add_executable(deengine main.cpp)
target_link_libraries(deengine PRIVATE de::core de::platform de::rhi de::render_graph de::shader de::compile_options)
# Dev builds mount the source tree as src:/ (shaders compile from it and hot reload).
target_compile_definitions(deengine PRIVATE "DE_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
de_deploy_slang_runtime(deengine)
