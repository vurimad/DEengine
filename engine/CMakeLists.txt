find_package(imgui CONFIG REQUIRED)

add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(asset)
add_subdirectory(render)
add_subdirectory(tools)
add_subdirectory(world)

# The one engine executable: game, editor (--editor) and headless (--headless) are modes of it.
add_executable(deengine main.cpp)
target_link_libraries(deengine PRIVATE de::asset de::core de::platform de::rhi de::render_graph de::render_imgui de::render_scene de::shader de::tools de::world de::compile_options imgui::imgui)
# Dev builds mount the source tree as src:/ (shaders compile from it and hot reload).
target_compile_definitions(deengine PRIVATE "DE_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
de_deploy_slang_runtime(deengine)
