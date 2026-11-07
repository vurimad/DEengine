find_package(yyjson CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(unofficial-ufbx CONFIG REQUIRED)
find_package(unofficial-bc7enc-rdo CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_path(CGLTF_INCLUDE_DIR cgltf.h REQUIRED)

# The asset cooker (docs/design.md, Asset formats): a library, so the editor can run it on file
# changes later and tests can drive it, and the cooker command line.
add_library(de_cooker STATIC
    blob_writer.cpp
    blob_writer.h
    cell_cooker.cpp
    cell_cooker.h
    collision_cooker.cpp
    collision_cooker.h
    common.cpp
    common.h
    cook_state.cpp
    cook_state.h
    cooker.cpp
    cooker.h
    fbx_import.cpp
    gltf_import.cpp
    json.cpp
    json.h
    material_cooker.cpp
    material_cooker.h
    mesh_cooker.cpp
    mesh_cooker.h
    model.cpp
    model.h
    settings.cpp
    settings.h
    shader_cooker.cpp
    shader_cooker.h
    source_tree.cpp
    source_tree.h
    terrain_cooker.cpp
    terrain_cooker.h
    texture_cooker.cpp
    texture_cooker.h
    world_cooker.cpp
    world_cooker.h
    third_party.cpp
    third_party.h)
add_library(de::cooker ALIAS de_cooker)

# Cooker headers are included relative to tools/: #include "cooker/cooker.h"
target_include_directories(de_cooker PUBLIC "${PROJECT_SOURCE_DIR}/tools")
target_include_directories(de_cooker SYSTEM PRIVATE "${Stb_INCLUDE_DIR}" "${CGLTF_INCLUDE_DIR}")
target_link_libraries(de_cooker
    PUBLIC de::asset de::core de::platform de::shader de::compile_options yyjson::yyjson
    PRIVATE de::physics_shapes meshoptimizer::meshoptimizer unofficial::ufbx::ufbx unofficial::bc7enc-rdo::bc7enc_rdo)

add_executable(cooker main.cpp)
target_link_libraries(cooker PRIVATE de::cooker)
target_compile_definitions(cooker PRIVATE "DE_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
de_deploy_slang_runtime(cooker)
