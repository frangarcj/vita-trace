set(VITA_APP_NAME "Vita Tracy Bringup")
set(VITA_TITLEID "VTRC00005")
set(VITA_VERSION "01.00")

add_executable(bringup_sample
    src/main.cpp
    ${CMAKE_SOURCE_DIR}/samples/common/debugScreen.c
)

target_include_directories(bringup_sample PRIVATE
    ${CMAKE_SOURCE_DIR}/samples/common
)

target_link_libraries(bringup_sample
    tracy_vita
    SceDisplay_stub
)

vita_create_self(bringup_sample.self bringup_sample UNSAFE)
vita_create_vpk(bringup_sample.vpk ${VITA_TITLEID} bringup_sample.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
