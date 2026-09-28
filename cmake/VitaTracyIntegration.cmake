# Link-time integration; no constructors before crt0 and no probing weak imports.
function(vita_tracy_enable target)
    cmake_parse_arguments(VT "PMU;FRAMES;PC_SAMPLING" "PMU_HZ;CORE_MASK;SAMPLE_HZ" "" ${ARGN})
    if(VT_UNPARSED_ARGUMENTS OR VT_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "vita_tracy_enable: unknown arguments or missing values")
    endif()
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "vita_tracy_enable: '${target}' is not a target")
    endif()
    get_target_property(_type "${target}" TYPE)
    get_target_property(_enabled "${target}" VITA_TRACY_AUTOSTART)
    if(NOT _type STREQUAL "EXECUTABLE" OR _enabled)
        message(FATAL_ERROR "vita_tracy_enable requires an executable not already enabled")
    endif()
    get_target_property(_manual tracy_vita VITA_TRACY_MANUAL_INIT)
    if(NOT _manual)
        message(FATAL_ERROR "vita_tracy_enable requires Tracy delayed/manual initialization")
    endif()
    if(NOT VT_PMU AND (DEFINED VT_PMU_HZ OR DEFINED VT_CORE_MASK))
        message(FATAL_ERROR "PMU_HZ and CORE_MASK require PMU")
    endif()
    if(VT_PMU AND VT_PC_SAMPLING)
        message(FATAL_ERROR "PMU and PC_SAMPLING are mutually exclusive: both own the hardware PMU")
    endif()
    if(NOT VT_PC_SAMPLING AND DEFINED VT_SAMPLE_HZ)
        message(FATAL_ERROR "SAMPLE_HZ requires PC_SAMPLING")
    endif()
    if(NOT DEFINED VT_PMU_HZ)
        set(VT_PMU_HZ 100)
    endif()
    if(NOT DEFINED VT_CORE_MASK)
        set(VT_CORE_MASK 7)
    endif()
    if(NOT DEFINED VT_SAMPLE_HZ)
        set(VT_SAMPLE_HZ 100)
    endif()
    if(NOT VT_PMU_HZ MATCHES "^[1-9][0-9]*$" OR VT_PMU_HZ LESS 10 OR VT_PMU_HZ GREATER 1000)
        message(FATAL_ERROR "PMU_HZ must be an integer from 10 to 1000")
    endif()
    if(NOT VT_CORE_MASK MATCHES "^[1-9][0-9]*$" OR VT_CORE_MASK GREATER 15)
        message(FATAL_ERROR "CORE_MASK must be a decimal bitmask from 1 to 15")
    endif()
    if(NOT VT_SAMPLE_HZ MATCHES "^[1-9][0-9]*$" OR VT_SAMPLE_HZ LESS 10 OR VT_SAMPLE_HZ GREATER 1000)
        message(FATAL_ERROR "SAMPLE_HZ must be an integer from 10 to 1000")
    endif()
    get_target_property(_root tracy_vita VITA_TRACY_SOURCE_ROOT)
    target_sources("${target}" PRIVATE
        "${_root}/client/tracy_vita_autostart.c"
        "${_root}/client/tracy_vita_autoemit.cpp"
    )
    target_compile_definitions("${target}" PRIVATE
        VITA_TRACY_AUTO_PMU=$<BOOL:${VT_PMU}>
        VITA_TRACY_AUTO_PC_SAMPLING=$<BOOL:${VT_PC_SAMPLING}>
        VITA_TRACY_AUTO_FRAMES=$<BOOL:${VT_FRAMES}>
        VITA_TRACY_AUTO_PMU_HZ=${VT_PMU_HZ}
        VITA_TRACY_AUTO_CORE_MASK=${VT_CORE_MASK}
        VITA_TRACY_AUTO_SAMPLE_HZ=${VT_SAMPLE_HZ}
    )
    target_link_options("${target}" PRIVATE "-Wl,--wrap=main" "-Wl,--wrap=sceKernelExitProcess")
    # Kernel-backed modes use tracy_vita's weak control-ABI import. A strong
    # import of this runtime-loaded syscall library hung process launch on
    # the 3.60 console (2026-09-22), so attach is gated on a marker file.
    target_link_libraries("${target}" PRIVATE tracy_vita)
    target_compile_options("${target}" PRIVATE -g)
    if(VT_FRAMES)
        target_link_options("${target}" PRIVATE "-Wl,--wrap=sceDisplaySetFrameBuf")
        target_link_libraries("${target}" PRIVATE SceDisplay_stub)
    endif()
    set_property(TARGET "${target}" PROPERTY VITA_TRACY_AUTOSTART TRUE)
endfunction()
