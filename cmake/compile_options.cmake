# Shared compile settings for every DEengine target (docs/design.md, Code conventions).
# Link de::compile_options instead of setting flags per target.
#
# Build configurations map to the doc's build kinds:
#   Debug          -> debug build (asserts, no optimization)
#   RelWithDebInfo -> dev build   (optimized, asserts, dev tools, symbols)
#   Release        -> ship build  (no asserts, no dev tools)

add_library(de_compile_options INTERFACE)
add_library(de::compile_options ALIAS de_compile_options)

target_compile_features(de_compile_options INTERFACE cxx_std_20)

target_compile_definitions(de_compile_options INTERFACE
    DE_BUILD_DEBUG=$<IF:$<CONFIG:Debug>,1,0>
    DE_BUILD_DEV=$<IF:$<CONFIG:Debug,RelWithDebInfo>,1,0>
    DE_BUILD_SHIP=$<IF:$<CONFIG:Release>,1,0>)

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # cl and clang-cl. CMake adds /EHsc and /GR by default; the engine uses neither
    # exceptions nor RTTI, so strip them instead of overriding them per target.
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

    target_compile_options(de_compile_options INTERFACE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /GR-
        /EHs-c-
        $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
        $<$<BOOL:${DE_WARNINGS_AS_ERRORS}>:/WX>)

    target_compile_definitions(de_compile_options INTERFACE
        _HAS_EXCEPTIONS=0
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
else()
    # clang and gcc
    target_compile_options(de_compile_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wimplicit-fallthrough
        -fno-exceptions
        -fno-rtti
        $<$<BOOL:${DE_WARNINGS_AS_ERRORS}>:-Werror>)
endif()
