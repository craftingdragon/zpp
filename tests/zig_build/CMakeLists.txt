cmake_minimum_required(VERSION 3.20)
project(zpp_zig_build_resource_test NONE)

include("${ZPP_MODULE}")

zpp_add_zig_build(resource_app
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    SOURCES src/main.zig
    RESOURCES src/assets/message.txt
)
