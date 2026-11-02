cmake_minimum_required(VERSION 3.20)
project(bc7enc_rdo CXX)

set(src "${CMAKE_CURRENT_SOURCE_DIR}/..")
add_library(bc7enc_rdo STATIC "${src}/bc7enc.cpp" "${src}/bc7decomp.cpp" "${src}/rgbcx.cpp")
target_compile_features(bc7enc_rdo PUBLIC cxx_std_11)
target_include_directories(bc7enc_rdo PUBLIC $<INSTALL_INTERFACE:include>)

install(TARGETS bc7enc_rdo EXPORT unofficial-bc7enc-rdo-targets ARCHIVE DESTINATION lib)
install(FILES "${src}/bc7enc.h" "${src}/bc7decomp.h" "${src}/rgbcx.h" DESTINATION include/bc7enc_rdo)
install(EXPORT unofficial-bc7enc-rdo-targets NAMESPACE unofficial::bc7enc-rdo:: DESTINATION share/unofficial-bc7enc-rdo)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/unofficial-bc7enc-rdo-config.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/unofficial-bc7enc-rdo-targets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/unofficial-bc7enc-rdo-config.cmake" DESTINATION share/unofficial-bc7enc-rdo)
