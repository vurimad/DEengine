# bc7enc_rdo isn't in vcpkg's registry: its block encoders and decoders (bc7enc, rgbcx, bc7decomp)
# are built here as a static library. The RDO encoder, the ISPC encoder and the command-line tool
# are left out.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO richgel999/bc7enc_rdo
    REF b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
    SHA512 ddb91ebcc2899fc81efdb53883c117653ffb4055049e4e7c41ee2c41635f70c5aa95e5c36e2dede7d8a541f5d9cbb06dc83fa116a5f04b0cf14b3beebfcb3f70
    HEAD_REF master
)

# The port's own build file replaces the repository's (which builds the tool).
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}/port")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}/port")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-bc7enc-rdo)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
