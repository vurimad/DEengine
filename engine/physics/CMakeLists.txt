find_package(Jolt CONFIG REQUIRED)

# Jolt on the engine's memory and jobs, stepping with the world (docs/design.md, Physics, camera, input & audio).
add_library(de_physics STATIC
    physics.cpp
    physics.h)
add_library(de::physics ALIAS de_physics)

# Private: Jolt's types stay in this library, and so do the build options it asks of what includes it.
target_link_libraries(de_physics
    PUBLIC de::core de::world de::compile_options
    PRIVATE de::platform Jolt::Jolt)
