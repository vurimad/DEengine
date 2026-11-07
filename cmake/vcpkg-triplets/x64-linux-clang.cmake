# x64-linux for the linux-clang preset. vcpkg builds ports with the system's default compiler, GCC, whatever the engine is
# built with. With Jolt built that way, clang's code tripped Jolt's IsNormalized assert on its first call to
# SetPositionAndRotation; with Jolt built by clang, it doesn't. Jolt's API passes vectors by value, so a difference in how
# the two compilers pass them is the likely reason, though that wasn't read from either's code. The other ports have no
# such types in their APIs and stay as they were.
include("${CMAKE_CURRENT_LIST_DIR}/x64-linux.cmake")

if(PORT STREQUAL "joltphysics")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
endif()

# vcpkg hashes this file, not what it includes.
list(APPEND VCPKG_HASH_ADDITIONAL_FILES "${CMAKE_CURRENT_LIST_DIR}/x64-linux.cmake")
