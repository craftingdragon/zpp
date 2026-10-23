# ZigPreprocessor.cmake
# Qt moc-style CMake integration for the standalone Zig preprocessor (zpp).
#
# Provides:
#   - zpp_wrap_zig(<out_var> SOURCES <zig_files...> [TARGET <target>] [BASE_DIR <dir>] [SHADOW_DIR <dir>] [ZPP_FLAGS <flags...>])
#   - zpp_target_sources(<target> ROOT <root.zig> [SOURCES <zig_files...>] [BASE_DIR <dir>] [ZIG_FLAGS <flags...>] [ZPP_FLAGS <flags...>])
#   - zpp_add_executable(<target> ROOT <root.zig> [SOURCES <zig_files...>] [C_SOURCES <c_files...>] ...)
#   - zpp_add_library(<target> [STATIC|SHARED|OBJECT] ROOT <root.zig> [SOURCES <zig_files...>] [C_SOURCES <c_files...>] ...)
#   - zpp_add_zig_build(<target> [SOURCE_DIR <dir>] [BUILD_ZIG <build.zig>] SOURCES <zig_files...> ...)

include_guard(GLOBAL)

find_program(ZIG_EXECUTABLE NAMES zig REQUIRED)

function(_zpp_get_tool_target out_var)
    if(TARGET zpp::zpp)
        set(${out_var} zpp::zpp PARENT_SCOPE)
    elseif(TARGET zpp)
        set(${out_var} zpp PARENT_SCOPE)
    else()
        find_program(ZPP_EXECUTABLE NAMES zpp REQUIRED)
        add_executable(zpp::zpp IMPORTED GLOBAL)
        set_target_properties(zpp::zpp PROPERTIES IMPORTED_LOCATION "${ZPP_EXECUTABLE}")
        set(${out_var} zpp::zpp PARENT_SCOPE)
    endif()
endfunction()

# Low-level helper analogous to qt_wrap_cpp():
# Preprocesses each input .zig file into a shadow build tree while preserving relative paths
# so Zig's @import("...") resolves sibling modules naturally.
function(zpp_wrap_zig out_shadow_files)
    set(options NO_LIBC NO_CIMPORT)
    set(oneValueArgs TARGET BASE_DIR SHADOW_DIR CIMPORT_DIR DEP_DIR TARGET_TRIPLE OUT_ROOT_SHADOW)
    set(multiValueArgs SOURCES INCLUDE_DIRECTORIES COMPILE_DEFINITIONS ZPP_FLAGS)
    cmake_parse_arguments(ZPP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ZPP_SOURCES)
        message(FATAL_ERROR "zpp_wrap_zig: SOURCES must be specified.")
    endif()

    _zpp_get_tool_target(ZPP_TOOL)

    if(NOT ZPP_BASE_DIR)
        set(ZPP_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    get_filename_component(ZPP_BASE_DIR "${ZPP_BASE_DIR}" ABSOLUTE)

    if(ZPP_TARGET)
        set(AUTOGEN_BASE "${CMAKE_CURRENT_BINARY_DIR}/${ZPP_TARGET}_zpp_autogen")
    else()
        set(AUTOGEN_BASE "${CMAKE_CURRENT_BINARY_DIR}/zpp_autogen")
    endif()

    if(NOT ZPP_SHADOW_DIR)
        set(ZPP_SHADOW_DIR "${AUTOGEN_BASE}/shadow")
    endif()
    if(NOT ZPP_CIMPORT_DIR)
        set(ZPP_CIMPORT_DIR "${ZPP_SHADOW_DIR}/__zpp_cimports")
    endif()
    if(NOT ZPP_DEP_DIR)
        set(ZPP_DEP_DIR "${AUTOGEN_BASE}/deps")
    endif()

    set(EXTRA_ARGS "")
    if(ZPP_NO_LIBC)
        list(APPEND EXTRA_ARGS "--no-libc")
    endif()
    if(ZPP_NO_CIMPORT)
        list(APPEND EXTRA_ARGS "--no-cimport")
    endif()
    if(ZPP_TARGET_TRIPLE)
        list(APPEND EXTRA_ARGS "--target=${ZPP_TARGET_TRIPLE}")
    endif()
    foreach(inc IN LISTS ZPP_INCLUDE_DIRECTORIES)
        list(APPEND EXTRA_ARGS "--include-dir=${inc}")
    endforeach()
    foreach(def IN LISTS ZPP_COMPILE_DEFINITIONS)
        list(APPEND EXTRA_ARGS "--define=${def}")
    endforeach()
    if(ZPP_ZPP_FLAGS)
        list(APPEND EXTRA_ARGS ${ZPP_ZPP_FLAGS})
    endif()

    # If bound to a CMake target, automatically forward its INCLUDE_DIRECTORIES and COMPILE_DEFINITIONS
    # via generator expressions (just like Qt's AUTOMOC).
    set(TARGET_GENEX_ARGS "")
    if(ZPP_TARGET AND TARGET "${ZPP_TARGET}")
        list(APPEND TARGET_GENEX_ARGS
            "--include-dir=$<TARGET_PROPERTY:${ZPP_TARGET},INCLUDE_DIRECTORIES>"
            "--define=$<TARGET_PROPERTY:${ZPP_TARGET},COMPILE_DEFINITIONS>"
        )
    endif()

    set(SHADOW_OUTPUTS "")
    set(FIRST_SHADOW "")

    foreach(src IN LISTS ZPP_SOURCES)
        get_filename_component(abs_src "${src}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        file(RELATIVE_PATH rel_src "${ZPP_BASE_DIR}" "${abs_src}")
        # Prevent '..' escapes if a file outside BASE_DIR was passed
        string(REPLACE ".." "__up__" safe_rel_src "${rel_src}")
        # Allow input files to use either .zig or .zpp extension; shadow outputs are always .zig
        string(REGEX REPLACE "\\.zpp$" ".zig" safe_rel_src "${safe_rel_src}")

        set(shadow_file "${ZPP_SHADOW_DIR}/${safe_rel_src}")
        set(dep_file "${ZPP_DEP_DIR}/${safe_rel_src}.d")
        get_filename_component(shadow_file_dir "${shadow_file}" DIRECTORY)
        get_filename_component(dep_file_dir "${dep_file}" DIRECTORY)

        add_custom_command(
            OUTPUT "${shadow_file}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${shadow_file_dir}" "${dep_file_dir}" "${ZPP_CIMPORT_DIR}"
            COMMAND "$<TARGET_FILE:${ZPP_TOOL}>"
                "--zig-executable=${ZIG_EXECUTABLE}"
                "--cimport-dir=${ZPP_CIMPORT_DIR}"
                "--depfile=${dep_file}"
                ${EXTRA_ARGS}
                ${TARGET_GENEX_ARGS}
                -o "${shadow_file}"
                "${abs_src}"
            DEPENDS "${abs_src}" "${ZPP_TOOL}"
            DEPFILE "${dep_file}"
            COMMENT "Generating shadow Zig source ${safe_rel_src} (zpp)"
            VERBATIM
        )

        list(APPEND SHADOW_OUTPUTS "${shadow_file}")
        if(NOT FIRST_SHADOW)
            set(FIRST_SHADOW "${shadow_file}")
        endif()
    endforeach()

    set(${out_shadow_files} "${SHADOW_OUTPUTS}" PARENT_SCOPE)
    if(ZPP_OUT_ROOT_SHADOW)
        set(${ZPP_OUT_ROOT_SHADOW} "${FIRST_SHADOW}" PARENT_SCOPE)
    endif()
endfunction()

# Attaches shadow Zig preprocessing and object compilation to an existing CMake target.
function(zpp_target_sources target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "zpp_target_sources: target '${target}' does not exist.")
    endif()

    set(options NO_LIBC NO_CIMPORT)
    set(oneValueArgs ROOT BASE_DIR TARGET_TRIPLE)
    set(multiValueArgs SOURCES INCLUDE_DIRECTORIES COMPILE_DEFINITIONS ZIG_FLAGS ZPP_FLAGS)
    cmake_parse_arguments(ZPP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ZPP_ROOT)
        message(FATAL_ERROR "zpp_target_sources(${target}): ROOT <root.zig> must be specified.")
    endif()

    if(NOT ZPP_BASE_DIR)
        get_filename_component(abs_root "${ZPP_ROOT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        get_filename_component(ZPP_BASE_DIR "${abs_root}" DIRECTORY)
    endif()

    set(ALL_ZIG_SOURCES "${ZPP_ROOT}")
    if(ZPP_SOURCES)
        list(APPEND ALL_ZIG_SOURCES ${ZPP_SOURCES})
    endif()
    list(REMOVE_DUPLICATES ALL_ZIG_SOURCES)

    set(WRAP_OPTS "")
    if(ZPP_NO_LIBC)
        list(APPEND WRAP_OPTS NO_LIBC)
    endif()
    if(ZPP_NO_CIMPORT)
        list(APPEND WRAP_OPTS NO_CIMPORT)
    endif()

    zpp_wrap_zig(SHADOW_ZIG_FILES
        TARGET "${target}"
        BASE_DIR "${ZPP_BASE_DIR}"
        OUT_ROOT_SHADOW SHADOW_ROOT_FILE
        TARGET_TRIPLE "${ZPP_TARGET_TRIPLE}"
        INCLUDE_DIRECTORIES ${ZPP_INCLUDE_DIRECTORIES}
        COMPILE_DEFINITIONS ${ZPP_COMPILE_DEFINITIONS}
        ZPP_FLAGS ${ZPP_ZPP_FLAGS}
        ${WRAP_OPTS}
        SOURCES ${ALL_ZIG_SOURCES}
    )

    set(AUTOGEN_BASE "${CMAKE_CURRENT_BINARY_DIR}/${target}_zpp_autogen")
    set(AUTOGEN_TARGET "${target}_zpp_autogen")
    if(NOT TARGET "${AUTOGEN_TARGET}")
        add_custom_target("${AUTOGEN_TARGET}" DEPENDS ${SHADOW_ZIG_FILES})
    endif()

    # Determine Zig optimization flag from CMake build type
    set(ZIG_OPT_FLAG "-O" "debug")
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(ZIG_OPT_FLAG "-O" "fast")
    elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set(ZIG_OPT_FLAG "-O" "safe")
    elseif(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        set(ZIG_OPT_FLAG "-O" "small")
    endif()

    set(ZIG_LIBC_FLAG "-lc")
    if(ZPP_NO_LIBC)
        set(ZIG_LIBC_FLAG "")
    endif()

    set(ZIG_TARGET_ARGS "")
    if(ZPP_TARGET_TRIPLE)
        list(APPEND ZIG_TARGET_ARGS "-target" "${ZPP_TARGET_TRIPLE}")
    endif()

    set(ZIG_OBJ_FILE "${AUTOGEN_BASE}/${target}_zig.o")
    set(ZIG_CACHE_DIR "${AUTOGEN_BASE}/zig-cache")

    add_custom_command(
        OUTPUT "${ZIG_OBJ_FILE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ZIG_CACHE_DIR}"
        COMMAND "${ZIG_EXECUTABLE}" build-obj
            "--cache-dir" "${ZIG_CACHE_DIR}"
            "-fPIC"
            ${ZIG_OPT_FLAG}
            ${ZIG_TARGET_ARGS}
            ${ZIG_LIBC_FLAG}
            ${ZPP_ZIG_FLAGS}
            "-Mroot=${SHADOW_ROOT_FILE}"
            "-femit-bin=${ZIG_OBJ_FILE}"
        DEPENDS "${SHADOW_ROOT_FILE}" ${SHADOW_ZIG_FILES} "${AUTOGEN_TARGET}"
        COMMENT "Compiling shadow Zig module for target ${target}"
        VERBATIM
    )

    set_source_files_properties("${ZIG_OBJ_FILE}" PROPERTIES
        EXTERNAL_OBJECT TRUE
        GENERATED TRUE
    )

    target_sources("${target}" PRIVATE "${ZIG_OBJ_FILE}")
    add_dependencies("${target}" "${AUTOGEN_TARGET}")

    get_target_property(CURRENT_LINKER_LANG "${target}" LINKER_LANGUAGE)
    if(NOT CURRENT_LINKER_LANG)
        set_target_properties("${target}" PROPERTIES LINKER_LANGUAGE C)
    endif()

    set_target_properties("${target}" PROPERTIES
        ZPP_SHADOW_DIR "${AUTOGEN_BASE}/shadow"
        ZPP_SHADOW_ROOT "${SHADOW_ROOT_FILE}"
    )
endfunction()

# High-level helper analogous to qt_add_executable():
# Creates a standard CMake executable target backed by a zpp shadow build.
function(zpp_add_executable target)
    set(options NO_LIBC NO_CIMPORT)
    set(oneValueArgs ROOT BASE_DIR TARGET_TRIPLE)
    set(multiValueArgs SOURCES C_SOURCES INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_LIBRARIES ZIG_FLAGS ZPP_FLAGS)
    cmake_parse_arguments(ZPP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ZPP_ROOT)
        message(FATAL_ERROR "zpp_add_executable(${target}): ROOT <root.zig> must be specified.")
    endif()

    add_executable("${target}" ${ZPP_C_SOURCES})

    if(ZPP_INCLUDE_DIRECTORIES)
        target_include_directories("${target}" PRIVATE ${ZPP_INCLUDE_DIRECTORIES})
    endif()
    if(ZPP_COMPILE_DEFINITIONS)
        target_compile_definitions("${target}" PRIVATE ${ZPP_COMPILE_DEFINITIONS})
    endif()
    if(ZPP_LINK_LIBRARIES)
        target_link_libraries("${target}" PRIVATE ${ZPP_LINK_LIBRARIES})
    endif()

    set(FWD_OPTS "")
    if(ZPP_NO_LIBC)
        list(APPEND FWD_OPTS NO_LIBC)
    endif()
    if(ZPP_NO_CIMPORT)
        list(APPEND FWD_OPTS NO_CIMPORT)
    endif()

    zpp_target_sources("${target}"
        ROOT "${ZPP_ROOT}"
        BASE_DIR "${ZPP_BASE_DIR}"
        TARGET_TRIPLE "${ZPP_TARGET_TRIPLE}"
        SOURCES ${ZPP_SOURCES}
        ZIG_FLAGS ${ZPP_ZIG_FLAGS}
        ZPP_FLAGS ${ZPP_ZPP_FLAGS}
        ${FWD_OPTS}
    )
endfunction()

# High-level helper analogous to qt_add_library():
# Creates a standard CMake library target (STATIC, SHARED, or OBJECT) backed by a zpp shadow build.
function(zpp_add_library target)
    set(options STATIC SHARED OBJECT NO_LIBC NO_CIMPORT)
    set(oneValueArgs ROOT BASE_DIR TARGET_TRIPLE)
    set(multiValueArgs SOURCES C_SOURCES INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_LIBRARIES ZIG_FLAGS ZPP_FLAGS)
    cmake_parse_arguments(ZPP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ZPP_ROOT)
        message(FATAL_ERROR "zpp_add_library(${target}): ROOT <root.zig> must be specified.")
    endif()

    set(LIB_TYPE STATIC)
    if(ZPP_SHARED)
        set(LIB_TYPE SHARED)
    elseif(ZPP_OBJECT)
        set(LIB_TYPE OBJECT)
    endif()

    add_library("${target}" ${LIB_TYPE} ${ZPP_C_SOURCES})

    if(ZPP_INCLUDE_DIRECTORIES)
        target_include_directories("${target}" PUBLIC ${ZPP_INCLUDE_DIRECTORIES})
    endif()
    if(ZPP_COMPILE_DEFINITIONS)
        target_compile_definitions("${target}" PUBLIC ${ZPP_COMPILE_DEFINITIONS})
    endif()
    if(ZPP_LINK_LIBRARIES)
        target_link_libraries("${target}" PUBLIC ${ZPP_LINK_LIBRARIES})
    endif()

    set(FWD_OPTS "")
    if(ZPP_NO_LIBC)
        list(APPEND FWD_OPTS NO_LIBC)
    endif()
    if(ZPP_NO_CIMPORT)
        list(APPEND FWD_OPTS NO_CIMPORT)
    endif()

    zpp_target_sources("${target}"
        ROOT "${ZPP_ROOT}"
        BASE_DIR "${ZPP_BASE_DIR}"
        TARGET_TRIPLE "${ZPP_TARGET_TRIPLE}"
        SOURCES ${ZPP_SOURCES}
        ZIG_FLAGS ${ZPP_ZIG_FLAGS}
        ZPP_FLAGS ${ZPP_ZPP_FLAGS}
        ${FWD_OPTS}
    )
endfunction()

# Mirrors an entire Zig project (including build.zig) into a shadow build directory,
# preprocesses all .zig files with zpp, and invokes `zig build` inside the shadow directory.
function(zpp_add_zig_build target)
    set(options NO_LIBC NO_CIMPORT)
    set(oneValueArgs SOURCE_DIR BUILD_ZIG TARGET_TRIPLE)
    set(multiValueArgs SOURCES INCLUDE_DIRECTORIES COMPILE_DEFINITIONS ZIG_BUILD_ARGS ZPP_FLAGS)
    cmake_parse_arguments(ZPP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ZPP_SOURCE_DIR)
        set(ZPP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    get_filename_component(ZPP_SOURCE_DIR "${ZPP_SOURCE_DIR}" ABSOLUTE)

    if(NOT ZPP_BUILD_ZIG)
        set(ZPP_BUILD_ZIG "${ZPP_SOURCE_DIR}/build.zig")
    endif()
    get_filename_component(ZPP_BUILD_ZIG "${ZPP_BUILD_ZIG}" ABSOLUTE)

    set(ALL_ZIG_FILES "${ZPP_BUILD_ZIG}")
    if(ZPP_SOURCES)
        list(APPEND ALL_ZIG_FILES ${ZPP_SOURCES})
    endif()
    list(REMOVE_DUPLICATES ALL_ZIG_FILES)

    set(AUTOGEN_BASE "${CMAKE_CURRENT_BINARY_DIR}/${target}_zpp_autogen")
    set(SHADOW_DIR "${AUTOGEN_BASE}/shadow")

    set(WRAP_OPTS "")
    if(ZPP_NO_LIBC)
        list(APPEND WRAP_OPTS NO_LIBC)
    endif()
    if(ZPP_NO_CIMPORT)
        list(APPEND WRAP_OPTS NO_CIMPORT)
    endif()

    zpp_wrap_zig(SHADOW_ZIG_FILES
        BASE_DIR "${ZPP_SOURCE_DIR}"
        SHADOW_DIR "${SHADOW_DIR}"
        CIMPORT_DIR "${SHADOW_DIR}/__zpp_cimports"
        DEP_DIR "${AUTOGEN_BASE}/deps"
        TARGET_TRIPLE "${ZPP_TARGET_TRIPLE}"
        INCLUDE_DIRECTORIES ${ZPP_INCLUDE_DIRECTORIES}
        COMPILE_DEFINITIONS ${ZPP_COMPILE_DEFINITIONS}
        ZPP_FLAGS ${ZPP_ZPP_FLAGS}
        ${WRAP_OPTS}
        SOURCES ${ALL_ZIG_FILES}
    )

    set(STAMP_FILE "${AUTOGEN_BASE}/${target}_zig_build.stamp")
    add_custom_command(
        OUTPUT "${STAMP_FILE}"
        COMMAND "${ZIG_EXECUTABLE}" build
            --build-file "${SHADOW_DIR}/build.zig"
            --cache-dir "${AUTOGEN_BASE}/zig-cache"
            --prefix "${AUTOGEN_BASE}/zig-out"
            ${ZPP_ZIG_BUILD_ARGS}
        COMMAND "${CMAKE_COMMAND}" -E touch "${STAMP_FILE}"
        WORKING_DIRECTORY "${SHADOW_DIR}"
        DEPENDS ${SHADOW_ZIG_FILES}
        COMMENT "Running zig build in shadow directory ${SHADOW_DIR}"
        VERBATIM
    )

    add_custom_target("${target}" ALL DEPENDS "${STAMP_FILE}")
    set_target_properties("${target}" PROPERTIES
        ZPP_SHADOW_DIR "${SHADOW_DIR}"
        ZPP_PREFIX_DIR "${AUTOGEN_BASE}/zig-out"
    )
endfunction()
