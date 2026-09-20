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
    pmu_core_test.cpp
    pmu_overflow_test.cpp
    control_gate_test.cpp
    clock_sync_test.cpp
    shared_ring_test.cpp
    shared_layout_test.cpp
    state_test.cpp
    timebase_test.cpp
    thread_registry_test.cpp
    client_lock_test.cpp
    ${VITA_TRACY_SOURCE_DIR}/common/thread_registry.c
    ${VITA_TRACY_SOURCE_DIR}/common/pmu_core.c
    ${VITA_TRACY_SOURCE_DIR}/common/pmu_overflow.c
    ${VITA_TRACY_SOURCE_DIR}/common/control_gate.c
    ${CMAKE_SOURCE_DIR}/common/shared_layout.c
    ${CMAKE_SOURCE_DIR}/common/shared_ring.c
    ${CMAKE_SOURCE_DIR}/kernel/state.c
    ${CMAKE_SOURCE_DIR}/client/clock_sync.c
    ${CMAKE_SOURCE_DIR}/client/pmu_delta.c
    ${CMAKE_SOURCE_DIR}/client/timebase_convert.c
)

target_include_directories(vita_tracy_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${VITA_TRACY_SOURCE_DIR}/client
)

target_link_libraries(vita_tracy_tests PRIVATE doctest::doctest)
target_compile_features(vita_tracy_tests PRIVATE cxx_std_17)

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(vita_tracy_tests)

# Exercise the real timer lifecycle code against deterministic failure
# injection. This validates control flow, not interrupt behavior on hardware.
add_executable(vita_tracy_timer_tests
    main.cpp tick_source_test.cpp
    ${VITA_TRACY_SOURCE_DIR}/kernel/tick_source.c
)
target_include_directories(vita_tracy_timer_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${VITA_TRACY_SOURCE_DIR}/include
    ${VITA_TRACY_SOURCE_DIR}/kernel
)
target_link_libraries(vita_tracy_timer_tests PRIVATE doctest::doctest)
target_compile_features(vita_tracy_timer_tests PRIVATE cxx_std_17)
doctest_discover_tests(vita_tracy_timer_tests)

add_executable(vita_tracy_pmu_session_tests
    main.cpp pmu_session_test.cpp
    ${VITA_TRACY_SOURCE_DIR}/kernel/pmu_session.c
    ${VITA_TRACY_SOURCE_DIR}/common/pmu_core.c
    ${VITA_TRACY_SOURCE_DIR}/common/control_gate.c
    ${VITA_TRACY_SOURCE_DIR}/common/shared_layout.c
    ${VITA_TRACY_SOURCE_DIR}/common/shared_ring.c
)
target_include_directories(vita_tracy_pmu_session_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${VITA_TRACY_SOURCE_DIR}/include
    ${VITA_TRACY_SOURCE_DIR}/kernel
)
target_link_libraries(vita_tracy_pmu_session_tests PRIVATE doctest::doctest)
target_compile_features(vita_tracy_pmu_session_tests PRIVATE cxx_std_17)
doctest_discover_tests(vita_tracy_pmu_session_tests)

add_executable(vita_tracy_irq_sampler_tests
    main.cpp sampler_irq_test.cpp
    ${VITA_TRACY_SOURCE_DIR}/kernel/sampler_irq.c
    ${VITA_TRACY_SOURCE_DIR}/common/pmu_overflow.c
    ${VITA_TRACY_SOURCE_DIR}/common/control_gate.c
    ${VITA_TRACY_SOURCE_DIR}/common/shared_layout.c
    ${VITA_TRACY_SOURCE_DIR}/common/shared_ring.c
)
target_include_directories(vita_tracy_irq_sampler_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${VITA_TRACY_SOURCE_DIR}/include
    ${VITA_TRACY_SOURCE_DIR}/kernel
)
target_link_libraries(vita_tracy_irq_sampler_tests PRIVATE doctest::doctest)
target_compile_definitions(vita_tracy_irq_sampler_tests PRIVATE VITA_TRACY_TESTING=1)
target_compile_features(vita_tracy_irq_sampler_tests PRIVATE cxx_std_17)
doctest_discover_tests(vita_tracy_irq_sampler_tests)

add_executable(vita_tracy_lifetime_tests
    main.cpp client_lifetime_test.cpp
    ${VITA_TRACY_SOURCE_DIR}/client/tracy_vita_init.cpp
)
target_include_directories(vita_tracy_lifetime_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${VITA_TRACY_SOURCE_DIR}/include
)
target_compile_definitions(vita_tracy_lifetime_tests PRIVATE TRACY_DELAYED_INIT TRACY_MANUAL_LIFETIME)
target_link_libraries(vita_tracy_lifetime_tests PRIVATE doctest::doctest)
target_compile_features(vita_tracy_lifetime_tests PRIVATE cxx_std_17)
doctest_discover_tests(vita_tracy_lifetime_tests)

foreach(_mode IN ITEMS 0 1)
    add_executable(vita_tracy_autostart_${_mode}_tests
        main.cpp autostart_test.cpp
        ${VITA_TRACY_SOURCE_DIR}/client/tracy_vita_autostart.c
    )
    target_include_directories(vita_tracy_autostart_${_mode}_tests PRIVATE ${VITA_TRACY_SOURCE_DIR}/include)
    target_compile_definitions(vita_tracy_autostart_${_mode}_tests PRIVATE
        VITA_TRACY_AUTO_PMU=${_mode}
        VITA_TRACY_AUTO_PC_SAMPLING=0
    )
    target_link_libraries(vita_tracy_autostart_${_mode}_tests PRIVATE doctest::doctest)
    target_compile_features(vita_tracy_autostart_${_mode}_tests PRIVATE cxx_std_17)
    doctest_discover_tests(vita_tracy_autostart_${_mode}_tests TEST_PREFIX "bootstrap.${_mode}.")
endforeach()

add_executable(vita_tracy_autostart_pc_tests
    main.cpp autostart_test.cpp
    ${VITA_TRACY_SOURCE_DIR}/client/tracy_vita_autostart.c
)
target_include_directories(vita_tracy_autostart_pc_tests PRIVATE ${VITA_TRACY_SOURCE_DIR}/include)
target_compile_definitions(vita_tracy_autostart_pc_tests PRIVATE
    VITA_TRACY_AUTO_PMU=0
    VITA_TRACY_AUTO_PC_SAMPLING=1
)
target_link_libraries(vita_tracy_autostart_pc_tests PRIVATE doctest::doctest)
target_compile_features(vita_tracy_autostart_pc_tests PRIVATE cxx_std_17)
doctest_discover_tests(vita_tracy_autostart_pc_tests TEST_PREFIX "bootstrap.pc.")

add_executable(vita_tracy_autoframe_tests
    main.cpp autoframe_test.cpp
    ${VITA_TRACY_SOURCE_DIR}/client/tracy_vita_autoemit.cpp
)
target_include_directories(vita_tracy_autoframe_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fakes)
target_compile_definitions(vita_tracy_autoframe_tests PRIVATE VITA_TRACY_AUTO_FRAMES=1)
target_link_libraries(vita_tracy_autoframe_tests PRIVATE doctest::doctest)
target_compile_features(vita_tracy_autoframe_tests PRIVATE cxx_std_17)
doctest_discover_tests(vita_tracy_autoframe_tests)

add_executable(vita_tracy_bridge_tests
    main.cpp bridge_failure_test.cpp
    ${VITA_TRACY_SOURCE_DIR}/client/tracy_vita_kernel_bridge.cpp
    ${VITA_TRACY_SOURCE_DIR}/client/clock_sync.c
    ${VITA_TRACY_SOURCE_DIR}/common/shared_layout.c
    ${VITA_TRACY_SOURCE_DIR}/common/shared_ring.c
    ${VITA_TRACY_SOURCE_DIR}/common/thread_registry.c
)
target_include_directories(vita_tracy_bridge_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fakes
    ${VITA_TRACY_SOURCE_DIR}/include
    ${VITA_TRACY_SOURCE_DIR}/client
)
target_link_libraries(vita_tracy_bridge_tests PRIVATE doctest::doctest)
target_compile_features(vita_tracy_bridge_tests PRIVATE cxx_std_17)
doctest_discover_tests(vita_tracy_bridge_tests PROPERTIES TIMEOUT 20)

# The offline symbolication tool is host-side Python; its tests run in the
# same suite so a change there fails the build like any other.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME symbol_map
        COMMAND ${Python3_EXECUTABLE} -B -m unittest discover -s ${CMAKE_SOURCE_DIR}/tools -p "test_*.py"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools
    )
else()
    message(STATUS "Python3 not found; skipping symbol_map tests")
endif()
