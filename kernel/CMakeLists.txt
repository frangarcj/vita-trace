add_executable(tracy_kernel
    main.c
    module_snapshot.c
    pmu.c
    process_events.c
    sampler_debug_fallback.c
    sampler_pamgr.c
    service.c
    shared_layout.c
    shared_ring.c
    state.c
)

target_include_directories(tracy_kernel PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(tracy_kernel PRIVATE
    -Wall -Wextra -O2 -fno-builtin
)

# Module enumeration lives in SceModulemgrForKernel, whose NIDs differ
# between firmware branches, so the stub is selected explicitly rather than
# assumed. 3.60 uses the unsuffixed stub; 3.63 and later use the _363 one.
set(VITA_TRACY_FIRMWARE "360" CACHE STRING "Target firmware branch (360 or 363)")
set_property(CACHE VITA_TRACY_FIRMWARE PROPERTY STRINGS 360 363)

if(VITA_TRACY_FIRMWARE STREQUAL "363")
    set(VITA_TRACY_MODULEMGR_STUB SceModulemgrForKernel_363_stub)
elseif(VITA_TRACY_FIRMWARE STREQUAL "360")
    set(VITA_TRACY_MODULEMGR_STUB SceModulemgrForKernel_stub)
else()
    message(FATAL_ERROR "Unsupported VITA_TRACY_FIRMWARE '${VITA_TRACY_FIRMWARE}'; expected 360 or 363")
endif()

target_link_libraries(tracy_kernel
    SceCpuForDriver_stub
    SceDebugForDriver_stub
    SceProcEventForDriver_stub
    SceSysclibForDriver_stub
    SceSysmemForDriver_stub
    SceThreadmgrForDriver_stub
    ${VITA_TRACY_MODULEMGR_STUB}
)

set_target_properties(tracy_kernel PROPERTIES
    LINK_FLAGS "-nostdlib"
)

vita_create_self(tracy_kernel.skprx tracy_kernel
    CONFIG exports.yml
    UNSAFE
)
