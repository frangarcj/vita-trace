add_library(tracy_vita STATIC
    ${VITA_TRACY_SOURCE_DIR}/third_party/tracy/public/TracyClient.cpp
    ${VITA_TRACY_SOURCE_DIR}/common/shared_layout.c
    ${VITA_TRACY_SOURCE_DIR}/common/shared_ring.c
    ${VITA_TRACY_SOURCE_DIR}/common/thread_registry.c
    ${VITA_TRACY_SOURCE_DIR}/common/arm_unwind.c
    ${VITA_TRACY_SOURCE_DIR}/common/stack_scan.c
    clock_sync.c
    pmu_delta.c
    timebase_convert.c
    tracy_vita_alloc.cpp
    tracy_vita_platform.cpp
    tracy_vita_pmu.cpp
    tracy_vita_init.cpp
    tracy_vita_kernel_bridge.cpp
    tracy_vita_time.cpp
    tracy_vita_threads.cpp
)

target_include_directories(tracy_vita PUBLIC
    ${VITA_TRACY_SOURCE_DIR}/include
    ${VITA_TRACY_SOURCE_DIR}/third_party/tracy/public
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

# Tracy's event queue allocates in blocks of BLOCK_SIZE items (32 bytes
# each) and starts with six of them: 12 MB of the newlib heap with the
# upstream 64K, plus a 2 MB block for every thread that emits. A port that
# sizes that heap to its own needs (geometrizer running Daytona USA) then runs
# out of memory. 4K items keeps the start under 1 MB; the queue grows on
# demand (patches/tracy/0004). Public: every TU including Tracy's headers
# must agree on the queue layout.
set(VITA_TRACY_QUEUE_BLOCK_SIZE "4096" CACHE STRING "Tracy event queue block size in items (power of two)")
target_compile_definitions(tracy_vita PUBLIC TRACY_QUEUE_BLOCK_SIZE=${VITA_TRACY_QUEUE_BLOCK_SIZE})

# The profiler's serial queues (locks, GPU and other serialized events) are
# FastVectors created with room for 1M items each: 64 MB of the heap before
# the application's main() ran, measured on the console with geometrizer.
# They grow on demand (patches/tracy/0005).
set(VITA_TRACY_SERIAL_QUEUE_PREALLOC "16384" CACHE STRING "Tracy serial queue items preallocated at startup")
target_compile_definitions(tracy_vita PRIVATE TRACY_SERIAL_QUEUE_PREALLOC=${VITA_TRACY_SERIAL_QUEUE_PREALLOC})

# The control ABI is imported weakly so an instrumented application still
# loads and runs when tracy_kernel.skprx is absent.
set(VITA_TRACY_KERNEL_STUB
    ${VITA_TRACY_BINARY_DIR}/kernel/tracy_kernel_stubs/libVitaTracyKernel_stub_weak.a)

target_link_libraries(tracy_vita PUBLIC
    ${VITA_TRACY_KERNEL_STUB}
    # Weak, because it genuinely is optional: the clock falls back to the
    # process timer when ScePerf cannot be loaded, and a non-weak import for
    # a module that may never arrive is a load-time hazard for no benefit.
    ScePerf_stub_weak
    SceNet_stub
    SceNetCtl_stub
    SceSysmodule_stub
    pthread
)

add_dependencies(tracy_vita tracy_kernel_stubs)

set_target_properties(tracy_vita PROPERTIES
    VITA_TRACY_SOURCE_ROOT "${VITA_TRACY_SOURCE_DIR}"
)
if(NOT VITA_TRACY_NO_DELAYED_INIT)
    set_property(TARGET tracy_vita PROPERTY VITA_TRACY_MANUAL_INIT TRUE)
endif()
include("${VITA_TRACY_SOURCE_DIR}/cmake/VitaTracyIntegration.cmake")
