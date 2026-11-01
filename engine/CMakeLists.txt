add_subdirectory(core)

# The one engine executable: game, editor (--editor) and headless (--headless) are modes of it.
add_executable(deengine main.cpp)
target_link_libraries(deengine PRIVATE de::core de::compile_options)
