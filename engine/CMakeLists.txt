find_package(imgui CONFIG REQUIRED)

add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(asset)
add_subdirectory(render)
add_subdirectory(tools)
add_subdirectory(world)
add_subdirectory(physics)
add_subdirectory(app)

# The one engine executable: game, editor (--editor) and headless (--headless) are modes of it.
add_executable(deengine main.cpp)
target_link_libraries(deengine PRIVATE de::app de::compile_options)
de_deploy_slang_runtime(deengine)
