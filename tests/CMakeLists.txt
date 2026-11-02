find_package(imgui CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(de_tests
    main.cpp
    core/allocator_test.cpp
    core/arena_test.cpp
    core/array_test.cpp
    core/assert_test.cpp
    core/enum_flags_test.cpp
    core/fixed_string_test.cpp
    core/frame_allocator_test.cpp
    core/function_ref_test.cpp
    core/hash_map_test.cpp
    core/inline_array_test.cpp
    core/job_system_test.cpp
    core/log_test.cpp
    core/memory_test.cpp
    core/pool_test.cpp
    core/result_test.cpp
    core/slot_map_test.cpp
    core/string_id_test.cpp
    core/test_support.h
    core/version_test.cpp
    core/work_stealing_deque_test.cpp
    platform/input_test.cpp
    platform/thread_test.cpp
    platform/vfs_test.cpp
    render/barrier_test.cpp
    render/bindless_test.cpp
    render/graph_test.cpp
    render/imgui_test.cpp
    render/memory_test.cpp
    render/mips_test.cpp
    render/rhi_test.cpp
    render/shader_test.cpp
    render/upload_test.cpp
    support/gpu.h
    support/readback.h
    support/sandbox.h
    support/shader_kit.h)

# Shared test helpers: #include "support/sandbox.h".
target_include_directories(de_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Platform tests never open a window or initialize SDL's video, so they run on headless CI.
target_link_libraries(de_tests PRIVATE de::core de::platform de::rhi de::render_graph de::render_imgui de::shader de::tools de::compile_options imgui::imgui doctest::doctest Threads::Threads)

# The engine builds without exceptions, so tests do too. REQUIRE failures terminate the run.
target_compile_definitions(de_tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

# Tests read environment variables (DE_REQUIRE_GPU) with std::getenv, which the MSVC CRT flags as unsafe.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_definitions(de_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The repository root, so tests compile the real shaders from src:/shaders/.
target_compile_definitions(de_tests PRIVATE "DE_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
de_deploy_slang_runtime(de_tests)

add_test(NAME de_tests COMMAND de_tests)
# Screen-capture tools hook every Vulkan program on a machine (OBS's capture layer slows these
# tests about threefold); tests are never worth capturing.
set_tests_properties(de_tests PROPERTIES ENVIRONMENT "DISABLE_VULKAN_OBS_CAPTURE=1")
