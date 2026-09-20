add_executable(automatic_sample src/main.c)
vita_tracy_enable(automatic_sample PMU)
vita_create_self(automatic_sample.self automatic_sample)
vita_create_vpk(automatic_sample.vpk VTRACE006 automatic_sample.self
    VERSION 01.00 NAME "Tracy automatic PMU")

add_executable(automatic_pc_sample src/main.c)
vita_tracy_enable(automatic_pc_sample PC_SAMPLING SAMPLE_HZ 100)
vita_create_self(automatic_pc_sample.self automatic_pc_sample)
vita_create_vpk(automatic_pc_sample.vpk VTRACE008 automatic_pc_sample.self
    VERSION 01.00 NAME "Tracy automatic PC")
