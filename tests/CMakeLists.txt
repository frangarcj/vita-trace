include(FetchContent)

FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(doctest)
unset(CMAKE_POLICY_VERSION_MINIMUM)

add_executable(vita_tracy_tests
    main.cpp
    shared_ring_test.cpp
    ${CMAKE_SOURCE_DIR}/kernel/shared_ring.c
)

target_include_directories(vita_tracy_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(vita_tracy_tests PRIVATE doctest::doctest)
target_compile_features(vita_tracy_tests PRIVATE cxx_std_17)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(vita_tracy_tests)
