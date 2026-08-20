cmake_minimum_required(VERSION 3.16)

option(VITA_TRACY_BUILD_HOST_TESTS "Build host-native unit tests instead of Vita targets" OFF)

if(NOT VITA_TRACY_BUILD_HOST_TESTS AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Please define VITASDK, or set VITA_TRACY_BUILD_HOST_TESTS=ON for the host test suite")
    endif()
endif()

project(vita_tracy C CXX)

if(VITA_TRACY_BUILD_HOST_TESTS)
    enable_testing()
    add_subdirectory(tests)
else()
    include("${VITASDK}/share/vita.cmake" REQUIRED)

    add_subdirectory(client)
    add_subdirectory(kernel)
    add_subdirectory(samples/zones)
endif()
