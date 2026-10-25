file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}/source")
file(COPY "${FIXTURE_DIR}/" DESTINATION "${TEST_DIR}/source")

set(source_dir "${TEST_DIR}/source")
set(build_dir "${TEST_DIR}/build")
set(shadow_dir "${build_dir}/resource_app_zpp_autogen/shadow")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${source_dir}" -B "${build_dir}"
        -G "${TEST_GENERATOR}"
        "-DZPP_MODULE=${ZPP_MODULE}"
        "-DZPP_EXECUTABLE=${ZPP_EXE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Fixture configure failed:\n${output}\n${error}")
endif()

function(check_build should_run)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${build_dir}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Fixture build failed:\n${output}\n${error}")
    endif()
    if(should_run AND NOT output MATCHES "Running zig build")
        message(FATAL_ERROR "Expected zig build to run:\n${output}\n${error}")
    endif()
    if(NOT should_run AND output MATCHES "Running zig build")
        message(FATAL_ERROR "Unexpected zig build rerun:\n${output}\n${error}")
    endif()
endfunction()

function(check_shadow_copy relative_path)
    file(READ "${source_dir}/${relative_path}" source_content)
    file(READ "${shadow_dir}/${relative_path}" shadow_content)
    if(NOT source_content STREQUAL shadow_content)
        message(FATAL_ERROR "Shadow copy differs from source: ${relative_path}")
    endif()
endfunction()

check_build(TRUE)
check_shadow_copy("build.zig.zon")
check_shadow_copy("src/assets/message.txt")
check_build(FALSE)

file(APPEND "${source_dir}/build.zig.zon" "\n")
check_build(TRUE)
check_shadow_copy("build.zig.zon")

file(APPEND "${source_dir}/src/assets/message.txt" "Updated resource.\n")
check_build(TRUE)
check_shadow_copy("src/assets/message.txt")
check_build(FALSE)
