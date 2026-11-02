add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(render)

# The one engine executable: game, editor (--editor) and headless (--headless) are modes of it.
add_executable(deengine main.cpp)
target_link_libraries(deengine PRIVATE de::core de::platform de::rhi de::compile_options)
