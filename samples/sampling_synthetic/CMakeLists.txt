set(VITA_APP_NAME "Vita Tracy Sampling")
set(VITA_TITLEID "VTRC00002")
set(VITA_VERSION "01.00")

add_executable(sampling_synthetic_sample
    src/main.cpp
)

target_link_libraries(sampling_synthetic_sample
    tracy_vita
)

vita_create_self(sampling_synthetic_sample.self sampling_synthetic_sample UNSAFE)
vita_create_vpk(sampling_synthetic_sample.vpk ${VITA_TITLEID} sampling_synthetic_sample.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
