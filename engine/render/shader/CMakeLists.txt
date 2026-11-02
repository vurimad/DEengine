find_package(slang CONFIG REQUIRED)

add_library(de_shader STATIC
    shader_compiler.cpp
    shader_compiler.h
    shader_library.cpp
    shader_library.h)
add_library(de::shader ALIAS de_shader)

target_link_libraries(de_shader
    PUBLIC de::core de::platform de::rhi de::compile_options
    PRIVATE slang::slang)

# Slang's interfaces are COM-style: objects free themselves in release() and are never deleted
# through an interface pointer, so the interfaces have no virtual destructor by design.
set_source_files_properties(shader_compiler.cpp PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-non-virtual-dtor>")

# Slang's runtime: the compiler library, its SPIR-V helpers and its precompiled standard modules,
# which it loads from beside slang.dll. On Linux it finds them in vcpkg's lib folder through the
# rpath; on Windows they are copied next to each executable that compiles shaders. slang-llvm
# (CPU targets, 80 MB) is left out.
function(de_deploy_slang_runtime target)
    if(NOT WIN32)
        return()
    endif()
    set(bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    file(GLOB modules LIST_DIRECTORIES true "${bin}/slang-standard-module-*")
    get_filename_component(modules_name "${modules}" NAME)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${bin}/slang.dll" "${bin}/slang-compiler.dll" "${bin}/slang-glslang.dll" "${bin}/slang-glsl-module.dll"
            "$<TARGET_FILE_DIR:${target}>"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${modules}" "$<TARGET_FILE_DIR:${target}>/${modules_name}"
        VERBATIM)
endfunction()
