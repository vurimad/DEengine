# Overlay of vcpkg's x64-windows triplet: same settings, plus per-port build options.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

include("${CMAKE_CURRENT_LIST_DIR}/port-options.cmake")
