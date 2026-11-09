# The console: named commands and variables run from text (the console window, --exec, the agents' socket).
add_library(de_console STATIC
    console.cpp
    console.h)
add_library(de::console ALIAS de_console)

target_link_libraries(de_console PUBLIC de::core de::compile_options)
