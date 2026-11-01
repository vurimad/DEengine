# Overlay of vcpkg's x64-linux triplet: same settings, plus per-port build options.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

include("${CMAKE_CURRENT_LIST_DIR}/port-options.cmake")
# vcpkg hashes the triplet file, not what it includes: without this, editing port options
# would not rebuild the affected ports.
set(VCPKG_HASH_ADDITIONAL_FILES "${CMAKE_CURRENT_LIST_DIR}/port-options.cmake")
