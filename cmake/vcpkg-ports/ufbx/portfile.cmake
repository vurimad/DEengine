# ufbx isn't in vcpkg's registry: built here as a static library from its one source file.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ufbx/ufbx
    REF "v${VERSION}"
    SHA512 783733311503b01854505342ad4d77cfdc8fb191b8c7a664c02b090e054420c391c28f63416f824bfb64ba1ffef6a7f2f5cadd6e185d851d45c708fab2f24b51
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-ufbx)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
