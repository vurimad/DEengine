find_package(Jolt CONFIG REQUIRED)

# What the cooker shares with the engine: Jolt's setup, and making the mesh shapes .dephys files hold. No world in it, and
# no Jolt types in its header. Linked by both, so both have the one build of it.
add_library(de_physics_shapes STATIC
    jolt_shapes.cpp
    jolt_shapes.h)
add_library(de::physics_shapes ALIAS de_physics_shapes)

target_link_libraries(de_physics_shapes
    PUBLIC de::core de::compile_options
    PRIVATE Jolt::Jolt)

# Jolt on the engine's memory and jobs, stepping with the world (docs/design.md, Physics, camera, input & audio).
add_library(de_physics STATIC
    physics.cpp
    physics.h)
add_library(de::physics ALIAS de_physics)

# Private: Jolt's types stay in this library, and so do the build options it asks of what includes it.
target_link_libraries(de_physics
    PUBLIC de::asset de::core de::physics_shapes de::world de::compile_options
    PRIVATE Jolt::Jolt)
