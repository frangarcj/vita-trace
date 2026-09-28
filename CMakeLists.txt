cmake_minimum_required(VERSION 3.16)

option(VITA_TRACY_BUILD_HOST_TESTS "Build host-native unit tests instead of Vita targets" OFF)

# Collect only while a viewer is attached. Without this Tracy buffers from
# process start, which on a console means the queue grows for as long as
# nobody is watching, and a capture is lost entirely if the app exits before
# anyone connects.
option(VITA_TRACY_ON_DEMAND "Only collect while a viewer is connected" ON)

if(NOT VITA_TRACY_BUILD_HOST_TESTS AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Please define VITASDK, or set VITA_TRACY_BUILD_HOST_TESTS=ON for the host test suite")
    endif()
endif()

project(vita_tracy C CXX)
set(VITA_TRACY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(VITA_TRACY_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(_vita_tracy_samples_default ON)
else()
    set(_vita_tracy_samples_default OFF)
endif()
option(VITA_TRACY_BUILD_SAMPLES "Build the standalone bring-up applications" ${_vita_tracy_samples_default})

if(VITA_TRACY_BUILD_HOST_TESTS)
    enable_testing()
    add_subdirectory(tests)
else()
    include("${VITASDK}/share/vita.cmake" REQUIRED)

    if(NOT EXISTS "${VITA_TRACY_SOURCE_DIR}/third_party/tracy/public/TracyClient.cpp")
        message(FATAL_ERROR "Tracy submodule is missing; run: git submodule update --init --recursive")
    endif()

    include(cmake/VitaTracyPatches.cmake)
    vita_tracy_apply_patches()

    add_subdirectory(kernel)
    add_subdirectory(client)
    if(VITA_TRACY_BUILD_SAMPLES)
    add_subdirectory(samples/bringup)
    add_subdirectory(samples/zones)
    add_subdirectory(samples/sampling_synthetic)
    add_subdirectory(samples/irq_sampling)
    add_subdirectory(samples/pmu_cache)
    add_subdirectory(samples/pmu_branch)
    add_subdirectory(samples/automatic)
    add_subdirectory(samples/pfm_probe)
    endif()
endif()
