add_executable(pfm_probe src/main.c)

target_compile_options(pfm_probe PRIVATE -Wall -Wextra -O2 -fno-builtin -mgeneral-regs-only)

target_link_libraries(pfm_probe
    SceCpuForDriver_stub
    SceDipswForDriver_stub
    SceIntrmgrForDriver_stub
    SceIofilemgrForDriver_stub
    SceSysclibForDriver_stub
    SceSysmemForDriver_stub
    SceThreadmgrForDriver_stub
)

set_target_properties(pfm_probe PROPERTIES LINK_FLAGS "-nostdlib")

vita_create_self(pfm_probe.skprx pfm_probe CONFIG exports.yml UNSAFE)
