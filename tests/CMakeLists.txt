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
    pmu_test.cpp
    clock_sync_test.cpp
    shared_ring_test.cpp
    shared_layout_test.cpp
    state_test.cpp
    timebase_test.cpp
    ${CMAKE_SOURCE_DIR}/common/shared_layout.c
    ${CMAKE_SOURCE_DIR}/common/shared_ring.c
    ${CMAKE_SOURCE_DIR}/kernel/state.c
    ${CMAKE_SOURCE_DIR}/client/clock_sync.c
    ${CMAKE_SOURCE_DIR}/client/pmu_delta.c
    ${CMAKE_SOURCE_DIR}/client/timebase_convert.c
)

target_include_directories(vita_tracy_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(vita_tracy_tests PRIVATE doctest::doctest)
target_compile_features(vita_tracy_tests PRIVATE cxx_std_17)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(vita_tracy_tests)

# The offline symbolication tool is host-side Python; its tests run in the
# same suite so a change there fails the build like any other.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME symbol_map
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/tools -p "test_*.py"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools
    )
else()
    message(STATUS "Python3 not found; skipping symbol_map tests")
endif()
