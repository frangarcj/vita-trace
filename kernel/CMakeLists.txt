enable_language(ASM)
add_executable(tracy_kernel
    main.c
    firmware_exports.c
    exports_taihen.c
    module_snapshot.c
    pmu_session.c
    pmu_arm.c
    pmu_thread_ctx.c
    process_events.c
    sampler.c
    sampler_debug_fallback.c
    sampler_irq.c
    irq_entry.S
    user_read.c
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
    # Code on the raw IRQ path must not reach SceSysclib's memcpy/memset, which
    # use d0-d3 (the interrupted thread's VFP state): no loop-to-memcpy idioms.
    -fno-tree-loop-distribute-patterns
)

# Start hardware bring-up on one CPU without editing the backend or its ABI.
set(VITA_TRACY_IRQ_CORE_MASK "7" CACHE STRING "PC sampler app-core mask (decimal 1..7)")
if(NOT VITA_TRACY_IRQ_CORE_MASK MATCHES "^[1-7]$")
    message(FATAL_ERROR "VITA_TRACY_IRQ_CORE_MASK must be a decimal mask from 1 to 7")
endif()
target_compile_definitions(tracy_kernel PRIVATE
    VITA_TRACY_IRQ_CORE_MASK=${VITA_TRACY_IRQ_CORE_MASK})

# Keep old build scripts usable; all known 360/363 exports now resolve at
# startup. No revision-specific ForKernel import can reject the ELF first.
set(VITA_TRACY_FIRMWARE "auto" CACHE STRING "Legacy compatibility hint (auto, 360 or 363)")
set_property(CACHE VITA_TRACY_FIRMWARE PROPERTY STRINGS auto 360 363)
if(NOT VITA_TRACY_FIRMWARE MATCHES "^(auto|360|363)$")
    message(FATAL_ERROR "Unsupported VITA_TRACY_FIRMWARE '${VITA_TRACY_FIRMWARE}'")
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
    SceIntrmgrForDriver_stub
    taihenModuleUtils_stub
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
