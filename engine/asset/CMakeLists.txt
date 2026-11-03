# Cooked asset formats (docs/design.md, Asset formats): what the cooker writes and the engine loads
# in place. The cooker (tools/cooker/) and the asset registry build on it.
add_library(de_asset STATIC
    asset_id.cpp
    asset_id.h
    format.cpp
    format.h
    material_format.cpp
    material_format.h
    mesh_format.cpp
    mesh_format.h
    shader_format.cpp
    shader_format.h
    texture_format.cpp
    texture_format.h)
add_library(de::asset ALIAS de_asset)

target_link_libraries(de_asset PUBLIC de::core de::compile_options)
