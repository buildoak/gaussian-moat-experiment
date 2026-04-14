cmake_minimum_required(VERSION 3.18)
project(tile_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# K_SQ parameterization: cmake -DK_SQ=36 .. (default 40)
if(NOT DEFINED K_SQ)
    set(K_SQ 40)
endif()
message(STATUS "Building tile-cpp with K_SQ=${K_SQ}")
add_compile_definitions(K_SQ_VAL=${K_SQ})

# Warnings
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# Release by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")

include_directories(include)

# Phase source files (each worker creates their own)
set(TILE_SOURCES
    src/sieve.cpp
    src/compact.cpp
    src/union_find.cpp
    src/face_extract.cpp
    src/prune.cpp
    src/encode.cpp
    src/process_tile.cpp
)

# Main library
add_library(tile STATIC ${TILE_SOURCES})

# End-to-end test
add_executable(test_e2e tests/test_e2e.cpp)
target_link_libraries(test_e2e tile)

add_executable(run_tile tests/run_tile.cpp)
target_link_libraries(run_tile tile)

# Per-phase test executables
add_executable(test_sieve tests/test_sieve.cpp)
target_link_libraries(test_sieve tile)

add_executable(test_compact_uf tests/test_compact_uf.cpp)
target_link_libraries(test_compact_uf tile)

add_executable(test_face_encode tests/test_face_encode.cpp)
target_link_libraries(test_face_encode tile)

add_executable(bench_tiles tests/bench_tiles.cpp)
target_link_libraries(bench_tiles tile)

add_executable(census_tiles tests/census_tiles.cpp)
target_link_libraries(census_tiles tile)

add_executable(dump_tileops tests/dump_tileops.cpp)
target_link_libraries(dump_tileops tile)
