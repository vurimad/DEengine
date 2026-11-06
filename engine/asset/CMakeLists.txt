# Cooked asset formats (docs/design.md, Asset formats): what the cooker writes and the engine loads
# in place, and the registry that loads them by ID. The cooker (tools/cooker/) builds on the formats.
add_library(de_asset STATIC
    asset_id.cpp
    asset_id.h
    cell_format.cpp
    cell_format.h
    collision_format.cpp
    collision_format.h
    environment_format.cpp
    environment_format.h
    format.cpp
    format.h
    material_format.cpp
    material_format.h
    mesh_format.cpp
    mesh_format.h
    registry.cpp
    registry.h
    shader_format.cpp
    shader_format.h
    texture_format.cpp
    texture_format.h
    world_format.cpp
    world_format.h)
add_library(de::asset ALIAS de_asset)

target_link_libraries(de_asset PUBLIC de::core de::platform de::compile_options)
