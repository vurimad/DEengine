cmake_minimum_required(VERSION 3.20)
project(ufbx C)

add_library(ufbx STATIC ufbx.c)
target_include_directories(ufbx PUBLIC $<INSTALL_INTERFACE:include>)

install(TARGETS ufbx EXPORT unofficial-ufbx-targets ARCHIVE DESTINATION lib)
install(FILES ufbx.h DESTINATION include)
install(EXPORT unofficial-ufbx-targets NAMESPACE unofficial::ufbx:: DESTINATION share/unofficial-ufbx)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/unofficial-ufbx-config.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/unofficial-ufbx-targets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/unofficial-ufbx-config.cmake" DESTINATION share/unofficial-ufbx)
