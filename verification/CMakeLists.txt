cmake_minimum_required(VERSION 3.22)
project(gaussian_moat_verification CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

add_library(verification_common INTERFACE)
target_include_directories(verification_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

foreach(tool
    bz_exact_check
    boundary_semantics_test
    exact_global_uf
    tile_sample_check
    span_cert_check)
  add_executable(${tool} src/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE verification_common nlohmann_json::nlohmann_json)
endforeach()

enable_testing()
add_test(NAME boundary_semantics_test COMMAND boundary_semantics_test)
add_test(NAME bz_exact_k36_lower_span
  COMMAND bz_exact_check --k-sq 36 --r-inner 73339843 --r-outer 73372611)
add_test(NAME bz_exact_k36_lower_moat
  COMMAND bz_exact_check --k-sq 36 --r-inner 73359375 --r-outer 73392143)
add_test(NAME exact_global_uf_tiny
  COMMAND exact_global_uf --k-sq 36 --r-inner 100 --r-outer 500 --expect SPANNING)
add_test(NAME no_campaign_includes
  COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/no_campaign_includes.cmake)
