add_library(tracy_vita STATIC
    ${CMAKE_SOURCE_DIR}/third_party/tracy/public/TracyClient.cpp
    ${CMAKE_SOURCE_DIR}/common/shared_layout.c
    ${CMAKE_SOURCE_DIR}/common/shared_ring.c
    clock_sync.c
    pmu_delta.c
    timebase_convert.c
    tracy_vita_alloc.cpp
    tracy_vita_platform.cpp
    tracy_vita_pmu.cpp
    tracy_vita_init.cpp
    tracy_vita_kernel_bridge.cpp
    tracy_vita_time.cpp
)

target_include_directories(tracy_vita PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/tracy/public
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# These take part in Tracy's configuration name mangling, so the library and
# everything linking it must agree on them.
target_compile_definitions(tracy_vita PUBLIC
    TRACY_ENABLE
    TRACY_PLATFORM_HEADER="tracy_vita_platform.hpp"
)

if(VITA_TRACY_ON_DEMAND)
    target_compile_definitions(tracy_vita PUBLIC TRACY_ON_DEMAND)
endif()

# Tracy would otherwise build its profiler from a static constructor, and its
# very first act is to read the clock. The clock is ScePerf, which is not
# loaded into a process until sceSysmoduleLoadModule runs, so that read lands
# on an unresolved import and branches to zero before main() is ever reached.
# Delaying construction lets vita_tracy_init load the module first.
if(NOT VITA_TRACY_NO_DELAYED_INIT)
  target_compile_definitions(tracy_vita PUBLIC
    TRACY_DELAYED_INIT
    TRACY_MANUAL_LIFETIME
  )
endif()

# Tracy's discovery broadcast, system tracing, native sampling and source
# transfer all assume APIs the Vita does not have; sampling and context
# switches arrive from the kernel backend instead, and symbols are resolved
# on the PC from the ELF.
target_compile_definitions(tracy_vita PRIVATE
    TRACY_NO_BROADCAST
    TRACY_ONLY_IPV4
    TRACY_NO_SYSTEM_TRACING
    TRACY_NO_SAMPLING
    TRACY_NO_CONTEXT_SWITCH
    TRACY_NO_CODE_TRANSFER
    TRACY_SYMBOL_OFFLINE_RESOLVE
)

# The control ABI is imported weakly so an instrumented application still
# loads and runs when tracy_kernel.skprx is absent.
set(VITA_TRACY_KERNEL_STUB
    ${CMAKE_BINARY_DIR}/kernel/tracy_kernel_stubs/libVitaTracyKernel_stub_weak.a)

target_link_libraries(tracy_vita PUBLIC
    ${VITA_TRACY_KERNEL_STUB}
    ScePerf_stub
    SceNet_stub
    SceNetCtl_stub
    SceSysmodule_stub
    pthread
)

add_dependencies(tracy_vita tracy_kernel_stubs)
