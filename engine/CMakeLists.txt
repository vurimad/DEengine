add_subdirectory(core)
add_subdirectory(platform)

# The one engine executable: game, editor (--editor) and headless (--headless) are modes of it.
add_executable(deengine main.cpp)
target_link_libraries(deengine PRIVATE de::core de::platform de::compile_options)
