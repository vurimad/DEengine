# x64-linux for the linux-clang preset. vcpkg builds ports with the system's default compiler, GCC, whatever the engine is
# built with. Jolt's API passes its vectors by value, and a call from clang's code into GCC's Jolt got them wrong: built by
# clang, it's one compiler on both sides of that call. The rest have no such types in their APIs.
include("${CMAKE_CURRENT_LIST_DIR}/x64-linux.cmake")

if(PORT STREQUAL "joltphysics")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
endif()

# vcpkg hashes this file, not what it includes.
list(APPEND VCPKG_HASH_ADDITIONAL_FILES "${CMAKE_CURRENT_LIST_DIR}/x64-linux.cmake")
