cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED K_SQ)
  set(K_SQ 40 CACHE STRING "Squared step bound for the campaign")
endif()

if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES 89 CACHE STRING "CUDA architectures")
endif()

project(cuda_campaign_v2_sqrt40 LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_EXTENSIONS OFF)

set(CAMPAIGN_BUILD_TESTS OFF CACHE BOOL "Do not build CPU campaign tests" FORCE)
add_subdirectory(../cpp-campaign-v2 cpp_campaign_v2_build EXCLUDE_FROM_ALL)

add_library(campaign_cuda STATIC
  src/stub_passthrough.cu
  src/constants_upload.cu
  src/kernel_sieve.cu
  src/kernel_mr.cu
  src/kernel_compact.cu
  src/kernel_uf_v2.cu
  src/kernel_geo_flags.cu
  src/kernel_face_encode_v2.cu
  src/kernel_face_sort_pack.cu
  src/host_driver.cpp
)
target_include_directories(campaign_cuda
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../cpp-campaign-v2/src
)
target_link_libraries(campaign_cuda
  PUBLIC
    campaign
)
target_compile_definitions(campaign_cuda
  PUBLIC
    K_SQ=${K_SQ}
)
set_target_properties(campaign_cuda PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
  CUDA_RESOLVE_DEVICE_SYMBOLS ON
)
set_source_files_properties(src/kernel_sieve.cu PROPERTIES
  COMPILE_FLAGS "--maxrregcount=40"
)
set_source_files_properties(src/kernel_compact.cu PROPERTIES
  COMPILE_FLAGS "--maxrregcount=32"
)
set_source_files_properties(src/kernel_uf_v2.cu PROPERTIES
  COMPILE_FLAGS "--maxrregcount=40"
)
set_source_files_properties(src/kernel_geo_flags.cu PROPERTIES
  COMPILE_FLAGS "--maxrregcount=40"
)
set_source_files_properties(src/kernel_face_encode_v2.cu PROPERTIES
  COMPILE_FLAGS "--maxrregcount=40"
)
set_source_files_properties(src/kernel_face_sort_pack.cu PROPERTIES
  COMPILE_FLAGS "--maxrregcount=40"
)

add_executable(cuda_vs_cpu_diff
  apps/cuda_vs_cpu_diff.cpp
)
target_include_directories(cuda_vs_cpu_diff
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../cpp-campaign-v2/src
)
target_link_libraries(cuda_vs_cpu_diff
  PRIVATE
    campaign_cuda
)
target_compile_definitions(cuda_vs_cpu_diff
  PRIVATE
    K_SQ=${K_SQ}
)

add_executable(campaign_main_cuda
  apps/campaign_main_cuda.cpp
)
target_include_directories(campaign_main_cuda
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../cpp-campaign-v2/src
)
target_link_libraries(campaign_main_cuda
  PRIVATE
    campaign_cuda
)
target_compile_definitions(campaign_main_cuda
  PRIVATE
    K_SQ=${K_SQ}
)

enable_testing()

add_library(k3k4_parity_support STATIC
  tests/support/k3k4_parity_support.cpp
)
target_include_directories(k3k4_parity_support
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../cpp-campaign-v2/src
)
target_link_libraries(k3k4_parity_support
  PUBLIC
    campaign_cuda
)
target_compile_definitions(k3k4_parity_support
  PRIVATE
    K_SQ=${K_SQ}
)

add_executable(test_stub_roundtrip
  tests/test_stub_roundtrip.cpp
)
target_link_libraries(test_stub_roundtrip
  PRIVATE
    campaign_cuda
)
target_compile_definitions(test_stub_roundtrip
  PRIVATE
    K_SQ=${K_SQ}
)

add_library(k5_parity_support STATIC
  tests/support/k5_parity_support.cpp
)
target_include_directories(k5_parity_support
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_link_libraries(k5_parity_support
  PUBLIC
    campaign_cuda
)
target_compile_definitions(k5_parity_support
  PRIVATE
    K_SQ=${K_SQ}
)

foreach(k5_test
    test_face_groups_parity
    test_port_sort_collision
    test_full_tileop_parity)
  add_executable(${k5_test}
    tests/${k5_test}.cpp
  )
  target_link_libraries(${k5_test}
    PRIVATE
      k5_parity_support
  )
  target_compile_definitions(${k5_test}
    PRIVATE
      K_SQ=${K_SQ}
  )
  add_test(NAME ${k5_test} COMMAND ${k5_test})
endforeach()

add_test(NAME test_stub_roundtrip COMMAND test_stub_roundtrip)
add_test(NAME cuda_vs_cpu_diff_one_tile COMMAND cuda_vs_cpu_diff --r-inner 100 --r-outer 500 --limit 1)
add_test(NAME campaign_main_cuda_small_octant
  COMMAND campaign_main_cuda
    --k-sq=${K_SQ}
    --r-inner 100
    --r-outer 500
    --region full-octant
    --out /tmp/campaign_main_cuda_small_octant.bin
    --chunk-size 64)

add_executable(test_k1_overflow_spanning
  tests/test_k1_overflow_spanning.cpp
)
target_link_libraries(test_k1_overflow_spanning
  PRIVATE
    campaign_cuda
)
target_compile_definitions(test_k1_overflow_spanning
  PRIVATE
    K_SQ=${K_SQ}
)
add_test(NAME test_k1_overflow_spanning COMMAND test_k1_overflow_spanning)

foreach(k3k4_test
    test_k3k4_parent_parity
    test_dense_remap_adversarial
    test_geo_i128_sweep)
  add_executable(${k3k4_test}
    tests/${k3k4_test}.cpp
  )
  target_link_libraries(${k3k4_test}
    PRIVATE
      k3k4_parity_support
  )
  target_include_directories(${k3k4_test}
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/../cpp-campaign-v2/src
  )
  target_compile_definitions(${k3k4_test}
    PRIVATE
      K_SQ=${K_SQ}
  )
  if(k3k4_test STREQUAL test_geo_i128_sweep)
    target_compile_definitions(${k3k4_test}
      PRIVATE
        CUDA_CAMPAIGN_ENABLE_GEO_GPU_TEST_API
    )
  endif()
  add_test(NAME ${k3k4_test} COMMAND ${k3k4_test})
endforeach()
