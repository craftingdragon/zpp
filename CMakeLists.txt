cmake_minimum_required(VERSION 3.20)
project(zig_preprocessor VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Core preprocessor library (kept separate from main.cpp for unit testing)
add_library(zpp_core STATIC
    src/lexer.cpp
    src/expr_eval.cpp
    src/preprocessor.cpp
    src/cimport.cpp
)
target_include_directories(zpp_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(zpp_core PRIVATE -Wall -Wextra -Wpedantic)

# Standalone zpp host executable (analogous to Qt's moc)
add_executable(zpp src/main.cpp)
target_link_libraries(zpp PRIVATE zpp_core)
target_compile_options(zpp PRIVATE -Wall -Wextra -Wpedantic)
add_executable(zpp::zpp ALIAS zpp)

# Export CMake integration module
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ZigPreprocessor)

# Install rules & CMake package export (supports `find_package(zpp REQUIRED)` system-wide)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(ZPP_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/zpp")

install(TARGETS zpp
    EXPORT zppTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(EXPORT zppTargets
    FILE zppTargets.cmake
    NAMESPACE zpp::
    DESTINATION "${ZPP_CMAKE_INSTALL_DIR}"
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/zppConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/zppConfig.cmake"
    INSTALL_DESTINATION "${ZPP_CMAKE_INSTALL_DIR}"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/zppConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ZigPreprocessor.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/zppConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/zppConfigVersion.cmake"
    DESTINATION "${ZPP_CMAKE_INSTALL_DIR}"
)

# Testing & demonstration targets
option(ZPP_BUILD_TESTS "Build zpp unit and shadow-build integration tests" ON)
if(ZPP_BUILD_TESTS)
    enable_testing()

    # 1. C++20 unit test suite for lexer, #if evaluator, macros, and @cImport
    add_executable(zpp_unit_tests tests/unit/test_preprocessor.cpp)
    target_link_libraries(zpp_unit_tests PRIVATE zpp_core)
    target_compile_options(zpp_unit_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME zpp_unit_tests COMMAND zpp_unit_tests)

    # 2. Sample C library to test CMake target_link_libraries + @cImport integration
    add_library(math_lib STATIC tests/c_lib/math_lib.c)
    target_include_directories(math_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tests/c_lib")
    target_compile_definitions(math_lib PUBLIC MATH_LIB_SCALE=3)

    # 3. Multi-file Zig application built via Qt moc-style shadow build
    zpp_add_executable(zig_shadow_app
        ROOT tests/zig_app/main.zig
        SOURCES
            tests/zig_app/main.zig
            tests/zig_app/sub/helper.zig
    )
    # Standard CMake target commands automatically propagate to zpp and zig translate-c
    target_link_libraries(zig_shadow_app PRIVATE math_lib)
    target_compile_definitions(zig_shadow_app PRIVATE
        APP_FEATURE_LEVEL=2
        USE_STDIO_IN_CIMPORT=1
    )

    add_test(NAME zig_shadow_app_test COMMAND zig_shadow_app)

    # 4. Preprocessed Zig static library linked into a C executable
    zpp_add_library(zig_calc_lib STATIC
        ROOT tests/zig_lib/calc.zig
        LINK_LIBRARIES math_lib
        COMPILE_DEFINITIONS CUSTOM_ZIG_OFFSET=1000
    )
    add_executable(c_calls_zig_test tests/zig_lib/caller.c)
    target_link_libraries(c_calls_zig_test PRIVATE zig_calc_lib)
    add_test(NAME c_calls_zig_test COMMAND c_calls_zig_test)
endif()
