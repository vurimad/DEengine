find_package(slang CONFIG REQUIRED)

add_library(de_shader STATIC
    shader_asset.h
    shader_compiler.cpp
    shader_compiler.h
    shader_library.cpp
    shader_library.h)
add_library(de::shader ALIAS de_shader)

target_link_libraries(de_shader
    PUBLIC de::asset de::core de::platform de::rhi de::compile_options
    PRIVATE slang::slang)

# Slang's interfaces are COM-style: objects free themselves in release() and are never deleted
# through an interface pointer, so the interfaces have no virtual destructor by design.
set_source_files_properties(shader_compiler.cpp PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU>:-Wno-non-virtual-dtor>")

# Slang's runtime: the compiler library, its SPIR-V helpers and its precompiled standard modules,
# which it loads from beside slang.dll. On Linux it finds them in vcpkg's lib folder through the
# rpath; on Windows they are copied next to the executables that compile shaders, which share a
# folder: by one target they all depend on, since copying it after each link collides in a parallel
# build. slang-llvm (CPU targets, 80 MB) is left out.
function(de_deploy_slang_runtime target)
    if(NOT WIN32)
        return()
    endif()
    if(NOT TARGET de_slang_runtime)
        set(bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
        file(GLOB modules LIST_DIRECTORIES true "${bin}/slang-standard-module-*")
        get_filename_component(modules_name "${modules}" NAME)
        set(destination "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>")
        add_custom_target(de_slang_runtime
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${destination}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${bin}/slang.dll" "${bin}/slang-compiler.dll" "${bin}/slang-glslang.dll" "${bin}/slang-glsl-module.dll"
                "${destination}"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                "${modules}" "${destination}/${modules_name}"
            VERBATIM)
    endif()
    add_dependencies(${target} de_slang_runtime)
endfunction()
