set(VITA_APP_NAME "Vita Tracy Zones")
set(VITA_TITLEID "VTRC00001")
set(VITA_VERSION "01.00")

add_executable(zones_sample
    src/main.cpp
)

target_link_libraries(zones_sample
    tracy_vita
)

vita_create_self(zones_sample.self zones_sample UNSAFE)
vita_create_vpk(zones_sample.vpk ${VITA_TITLEID} zones_sample.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
