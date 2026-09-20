add_executable(tracy_kernel
    main.c
    module_snapshot.c
    pmu_session.c
    pmu_arm.c
    process_events.c
    sampler.c
    sampler_debug_fallback.c
    sampler_irq.c
    sampler_pamgr.c
    service.c
    tick_source.c
    ${VITA_TRACY_SOURCE_DIR}/common/shared_layout.c
    ${VITA_TRACY_SOURCE_DIR}/common/shared_ring.c
    ${VITA_TRACY_SOURCE_DIR}/common/thread_registry.c
    ${VITA_TRACY_SOURCE_DIR}/common/pmu_core.c
    ${VITA_TRACY_SOURCE_DIR}/common/pmu_overflow.c
    ${VITA_TRACY_SOURCE_DIR}/common/control_gate.c
    state.c
)

target_include_directories(tracy_kernel PRIVATE
    ${VITA_TRACY_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(tracy_kernel PRIVATE
    -Wall -Wextra -O2 -fno-builtin
    -mgeneral-regs-only -fno-tree-vectorize -fno-tree-slp-vectorize
)

# Start hardware bring-up on one CPU without editing the backend or its ABI.
set(VITA_TRACY_IRQ_CORE_MASK "7" CACHE STRING "PC sampler app-core mask (decimal 1..7)")
if(NOT VITA_TRACY_IRQ_CORE_MASK MATCHES "^[1-7]$")
    message(FATAL_ERROR "VITA_TRACY_IRQ_CORE_MASK must be a decimal mask from 1 to 7")
endif()
target_compile_definitions(tracy_kernel PRIVATE
    VITA_TRACY_IRQ_CORE_MASK=${VITA_TRACY_IRQ_CORE_MASK})

# Module enumeration lives in SceModulemgrForKernel, whose NIDs differ
# between firmware branches, so the stub is selected explicitly rather than
# assumed. 3.60 uses the unsuffixed stub; 3.63 and later use the _363 one.
set(VITA_TRACY_FIRMWARE "360" CACHE STRING "Target firmware branch (360 or 363)")
set_property(CACHE VITA_TRACY_FIRMWARE PROPERTY STRINGS 360 363)

if(VITA_TRACY_FIRMWARE STREQUAL "363")
    set(VITA_TRACY_MODULEMGR_STUB SceModulemgrForKernel_363_stub)
    set(VITA_TRACY_EXCPMGR_STUB SceExcpmgrForKernel_363_stub)
    set(VITA_TRACY_THREADMGR_KERNEL_STUB SceThreadmgrForKernel_363_stub)
elseif(VITA_TRACY_FIRMWARE STREQUAL "360")
    set(VITA_TRACY_MODULEMGR_STUB SceModulemgrForKernel_stub)
    set(VITA_TRACY_EXCPMGR_STUB SceExcpmgrForKernel_stub)
    set(VITA_TRACY_THREADMGR_KERNEL_STUB SceThreadmgrForKernel_stub)
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
    SceSystimerForDriver_stub
    ScePowerForDriver_stub
    ${VITA_TRACY_EXCPMGR_STUB}
    ${VITA_TRACY_THREADMGR_KERNEL_STUB}
    ${VITA_TRACY_MODULEMGR_STUB}
)

set_target_properties(tracy_kernel PROPERTIES
    LINK_FLAGS "-nostdlib"
)

vita_create_self(tracy_kernel.skprx tracy_kernel
    CONFIG exports.yml
    UNSAFE
)

# Userland calls the control ABI through syscalls, so the stub library is
# generated from the same exports the module publishes.
vita_create_stubs(tracy_kernel_stubs tracy_kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml
    KERNEL
)
