cmake_minimum_required(VERSION 3.18)

project(tiles-compositor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(compositor STATIC
    src/grid.cpp
    src/tileop_parse.cpp
    src/compositor.cpp
)

target_include_directories(compositor PUBLIC include)

target_compile_options(compositor
    PRIVATE
        -Wall
        -Wextra
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-march=native>
        $<$<CONFIG:Release>:-flto>
        $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

target_link_options(compositor
    PRIVATE
        $<$<CONFIG:Release>:-flto>
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

add_executable(test_grid test/test_grid.cpp)
target_link_libraries(test_grid PRIVATE compositor)
target_compile_options(test_grid PRIVATE -Wall -Wextra)

add_executable(test_tileop test/test_tileop.cpp)
target_link_libraries(test_tileop PRIVATE compositor)
target_compile_options(test_tileop PRIVATE -Wall -Wextra)

add_executable(test_compositor test/test_compositor.cpp)
target_link_libraries(test_compositor PRIVATE compositor)
target_compile_options(test_compositor PRIVATE -Wall -Wextra)

add_executable(test_compositor_real test/test_compositor_real.cpp)
target_link_libraries(test_compositor_real PRIVATE compositor)
target_compile_options(test_compositor_real PRIVATE -Wall -Wextra)

add_executable(test_compositor_octant test/test_compositor_octant.cpp)
target_link_libraries(test_compositor_octant PRIVATE compositor)
target_compile_options(test_compositor_octant PRIVATE -Wall -Wextra)

add_executable(campaign src/campaign.cpp)
target_link_libraries(campaign PRIVATE compositor)
target_compile_options(campaign PRIVATE -Wall -Wextra)
