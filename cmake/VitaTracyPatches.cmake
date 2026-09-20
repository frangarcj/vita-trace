# Applies the local patches to the pinned Tracy submodule.
#
# Tracy exposes platform hooks for its allocator, thread ids and user info,
# but not for the clock, so the Vita timebase is wired in through a patch
# that adds a TRACY_PLATFORM_GET_TIME hook alongside the existing
# TRACY_PLATFORM_HEADER mechanism. Re-running configure must be harmless,
# so each patch is skipped when it already applies in reverse.

function(vita_tracy_apply_patches)
    find_package(Git QUIET REQUIRED)

    file(GLOB patch_files "${VITA_TRACY_SOURCE_DIR}/patches/tracy/*.patch")
    list(SORT patch_files)

    foreach(patch ${patch_files})
        get_filename_component(patch_name "${patch}" NAME)

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch}"
            WORKING_DIRECTORY "${VITA_TRACY_SOURCE_DIR}/third_party/tracy"
            RESULT_VARIABLE already_applied
            OUTPUT_QUIET ERROR_QUIET
        )

        if(already_applied EQUAL 0)
            message(STATUS "Tracy patch already applied: ${patch_name}")
            continue()
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
            WORKING_DIRECTORY "${VITA_TRACY_SOURCE_DIR}/third_party/tracy"
            RESULT_VARIABLE apply_result
            ERROR_VARIABLE apply_error
        )

        if(NOT apply_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply Tracy patch ${patch_name}: ${apply_error}")
        endif()

        message(STATUS "Applied Tracy patch: ${patch_name}")
    endforeach()
endfunction()
