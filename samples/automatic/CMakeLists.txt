add_executable(automatic_sample src/main.c)
vita_tracy_enable(automatic_sample PMU)
vita_create_self(automatic_sample.self automatic_sample)
vita_create_vpk(automatic_sample.vpk VTRACE006 automatic_sample.self
    VERSION 01.00 NAME "Tracy automatic PMU")
