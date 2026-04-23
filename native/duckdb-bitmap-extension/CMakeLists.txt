cmake_minimum_required(VERSION 3.21)

project(duckdb_bitmap_extension LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(bitmap SHARED
    src/bitmap_extension.c
)

target_include_directories(bitmap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb_capi
)

target_compile_definitions(bitmap PRIVATE
    DUCKDB_EXTENSION_NAME=bitmap
    DUCKDB_EXTENSION_API_VERSION_MAJOR=1
    DUCKDB_EXTENSION_API_VERSION_MINOR=2
    DUCKDB_EXTENSION_API_VERSION_PATCH=0
)

if(MSVC)
    target_compile_definitions(bitmap PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set_target_properties(bitmap PROPERTIES
    OUTPUT_NAME bitmap
    PREFIX ""
)
