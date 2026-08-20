add_library(tracy_vita STATIC
    ${CMAKE_SOURCE_DIR}/third_party/tracy/public/TracyClient.cpp
    ${CMAKE_SOURCE_DIR}/common/shared_layout.c
    ${CMAKE_SOURCE_DIR}/common/shared_ring.c
    clock_sync.c
    timebase_convert.c
    tracy_vita_alloc.cpp
    tracy_vita_platform.cpp
    tracy_vita_init.cpp
    tracy_vita_kernel_bridge.cpp
    tracy_vita_time.cpp
)

target_include_directories(tracy_vita PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/tracy/public
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(tracy_vita PUBLIC
    TRACY_ENABLE
    TRACY_PLATFORM_HEADER="tracy_vita_platform.hpp"
)

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
