set(VITA_APP_NAME "Vita Tracy IRQ sampler")
set(VITA_TITLEID "VTRC00007")
set(VITA_VERSION "01.00")

add_executable(irq_sampling_sample
    src/main.cpp
    ${VITA_TRACY_SOURCE_DIR}/samples/common/debugScreen.c
)

target_include_directories(irq_sampling_sample PRIVATE
    ${VITA_TRACY_SOURCE_DIR}/samples/common
)

get_target_property(_vita_tracy_kernel_stub tracy_vita VITA_TRACY_REQUIRED_KERNEL_STUB)
target_link_libraries(irq_sampling_sample PRIVATE
    "-Wl,--whole-archive"
    "${_vita_tracy_kernel_stub}"
    "-Wl,--no-whole-archive"
    tracy_vita
    SceDisplay_stub
)
add_dependencies(irq_sampling_sample tracy_kernel_stubs)

vita_create_self(irq_sampling_sample.self irq_sampling_sample UNSAFE)
vita_create_vpk(irq_sampling_sample.vpk ${VITA_TITLEID} irq_sampling_sample.self
    VERSION ${VITA_VERSION}
    NAME "${VITA_APP_NAME}"
)
