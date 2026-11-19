find_package(flecs CONFIG REQUIRED)

# The game world: flecs, set up with the engine's memory, log and jobs (docs/design.md, Core systems).
add_library(de_world STATIC
    cell.cpp
    cell.h
    flecs_os.cpp
    flecs_os.h
    prefab.cpp
    prefab.h
    render_extract.cpp
    render_extract.h
    terrain_heights.cpp
    terrain_heights.h
    transform.cpp
    transform.h
    world.cpp
    world.h)
add_library(de::world ALIAS de_world)

target_link_libraries(de_world
    PUBLIC de::asset de::core de::platform de::compile_options $<IF:$<TARGET_EXISTS:flecs::flecs>,flecs::flecs,flecs::flecs_static>)
