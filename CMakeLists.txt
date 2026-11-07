cmake_minimum_required(VERSION 3.28)

project(DEengine
    VERSION 0.1.0
    DESCRIPTION "Open-world action RPG engine"
    LANGUAGES C CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use a preset: cmake --preset <name>")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# No C++20 modules: skip the per-file module dependency scan CMake does by default.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Executables and DLLs side by side: build/<preset>/bin/<config>/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

option(DE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(DE_BUILD_TESTS "Build the unit test suite" ON)

include(cmake/compile_options.cmake)

add_subdirectory(engine)
add_subdirectory(tools/cooker)
add_subdirectory(tools/shader_hash)
add_subdirectory(tools/image_diff)

if(DE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
