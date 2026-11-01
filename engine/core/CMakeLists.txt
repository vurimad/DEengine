add_library(de_core STATIC
    version.cpp)
add_library(de::core ALIAS de_core)

# Engine headers are included relative to engine/: #include "core/version.h"
target_include_directories(de_core PUBLIC "${PROJECT_SOURCE_DIR}/engine")
target_link_libraries(de_core PUBLIC de::compile_options)

target_compile_definitions(de_core PRIVATE
    DE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    DE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    DE_VERSION_PATCH=${PROJECT_VERSION_PATCH})
