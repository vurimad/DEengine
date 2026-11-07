find_package(Stb REQUIRED)

# Compares two PNGs: a library, so tests can drive it, and its command line.
add_library(de_image_diff STATIC
    image_diff.cpp
    image_diff.h)
add_library(de::image_diff ALIAS de_image_diff)

# Headers are included relative to tools/: #include "image_diff/image_diff.h"
target_include_directories(de_image_diff PUBLIC "${PROJECT_SOURCE_DIR}/tools")
target_include_directories(de_image_diff SYSTEM PRIVATE "${Stb_INCLUDE_DIR}")
target_link_libraries(de_image_diff PUBLIC de::core de::platform de::compile_options)

add_executable(image_diff main.cpp)
target_link_libraries(image_diff PRIVATE de::image_diff)
