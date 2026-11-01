find_package(doctest CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(de_tests
    main.cpp
    core/allocator_test.cpp
    core/arena_test.cpp
    core/array_test.cpp
    core/assert_test.cpp
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
    platform/vfs_test.cpp)

# Platform tests never open a window or initialize SDL's video, so they run on headless CI.
target_link_libraries(de_tests PRIVATE de::core de::platform de::compile_options doctest::doctest Threads::Threads)

# The engine builds without exceptions, so tests do too. REQUIRE failures terminate the run.
target_compile_definitions(de_tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

add_test(NAME de_tests COMMAND de_tests)
