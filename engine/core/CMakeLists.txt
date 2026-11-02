find_package(Threads REQUIRED)
find_package(Tracy CONFIG REQUIRED)

add_library(de_core STATIC
    allocator.cpp
    allocator.h
    arena.cpp
    arena.h
    array.h
    assert.cpp
    assert.h
    compiler.h
    detail/std_allocator.h
    detail/work_stealing_deque.h
    enum_flags.h
    fixed_string.h
    frame_allocator.cpp
    frame_allocator.h
    function_ref.h
    hash.h
    hash_map.h
    inline_array.h
    job_system.cpp
    job_system.h
    log.cpp
    log.h
    memory_stats.h
    memory_tag.h
    pool.cpp
    pool.h
    profile.h
    result.h
    slot_map.h
    string_id.h
    version.cpp
    version.h)
add_library(de::core ALIAS de_core)

# Engine headers are included relative to engine/: #include "core/version.h"
target_include_directories(de_core PUBLIC "${PROJECT_SOURCE_DIR}/engine")
target_link_libraries(de_core PUBLIC de::compile_options Threads::Threads)

# The Tracy profiler client, in debug and dev builds only: it brings TRACY_ENABLE, which turns
# the DE_PROFILE_* macros on (core/profile.h). Ship builds contain no profiler code or thread.
target_link_libraries(de_core PUBLIC $<$<CONFIG:Debug,RelWithDebInfo>:Tracy::TracyClient>)

target_compile_definitions(de_core PRIVATE
    DE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    DE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    DE_VERSION_PATCH=${PROJECT_VERSION_PATCH})

# Debugger views: embedded into the PDB of every executable that links core.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_link_options(de_core INTERFACE "/NATVIS:${CMAKE_CURRENT_SOURCE_DIR}/core.natvis")
endif()
