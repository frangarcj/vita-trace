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

# Weak control-ABI import through tracy_vita, exactly like the bring-up
# sample. Two console runs on 2026-09-22 with the strong (whole-archive)
# stub hung the console hard before the first report line after attach,
# while the bring-up's attach on the same kernel build succeeded; the
# strong import of a runtime-loaded syscall library is the prime suspect.
target_link_libraries(irq_sampling_sample PRIVATE
    tracy_vita
    SceDisplay_stub
)

vita_create_self(irq_sampling_sample.self irq_sampling_sample UNSAFE)
vita_create_vpk(irq_sampling_sample.vpk ${VITA_TITLEID} irq_sampling_sample.self
    VERSION ${VITA_VERSION}
    NAME "${VITA_APP_NAME}"
)
