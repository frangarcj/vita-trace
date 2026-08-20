set(VITA_APP_NAME "Vita Tracy PMU Branch")
set(VITA_TITLEID "VTRC00004")
set(VITA_VERSION "01.00")

add_executable(pmu_branch_sample
    src/main.cpp
)

target_link_libraries(pmu_branch_sample
    tracy_vita
)

vita_create_self(pmu_branch_sample.self pmu_branch_sample UNSAFE)
vita_create_vpk(pmu_branch_sample.vpk ${VITA_TITLEID} pmu_branch_sample.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
