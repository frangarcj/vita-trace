set(VITA_APP_NAME "Vita Tracy PMU Cache")
set(VITA_TITLEID "VTRC00003")
set(VITA_VERSION "01.00")

add_executable(pmu_cache_sample
    src/main.cpp
)

target_link_libraries(pmu_cache_sample
    tracy_vita
)

vita_create_self(pmu_cache_sample.self pmu_cache_sample UNSAFE)
vita_create_vpk(pmu_cache_sample.vpk ${VITA_TITLEID} pmu_cache_sample.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
