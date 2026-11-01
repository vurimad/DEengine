find_package(doctest CONFIG REQUIRED)

add_executable(de_tests
    main.cpp
    core/version_test.cpp)

target_link_libraries(de_tests PRIVATE de::core de::compile_options doctest::doctest)

# The engine builds without exceptions, so tests do too. REQUIRE failures terminate the run.
target_compile_definitions(de_tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

add_test(NAME de_tests COMMAND de_tests)
