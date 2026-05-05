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
find_package(Python3 COMPONENTS Interpreter)

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

add_executable(postflight_check postflight/src/postflight_check.cpp)
target_link_libraries(postflight_check PRIVATE verification_common nlohmann_json::nlohmann_json)

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
if(Python3_Interpreter_FOUND)
  add_test(NAME stats_normalize_sweep_rows_py
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/stats/test_normalize_sweep_rows.py)
endif()

add_test(NAME span_cert_fixture_valid
  COMMAND span_cert_check ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/span_cert/minimal-valid.json)
foreach(bad_span_cert
    bad-claim-id
    bad-metadata
    bad-point
    bad-region)
  add_test(NAME span_cert_fixture_${bad_span_cert}_reject
    COMMAND span_cert_check ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/span_cert/${bad_span_cert}.json)
  set_tests_properties(span_cert_fixture_${bad_span_cert}_reject PROPERTIES WILL_FAIL TRUE)
endforeach()

add_test(NAME tile_sample_fixture_valid
  COMMAND tile_sample_check
    --manifest ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tile_samples/valid-manifest.json
    --samples ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tile_samples/valid.tiles.jsonl)
add_test(NAME tile_sample_fixture_exhausted_quota_valid
  COMMAND tile_sample_check
    --manifest ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tile_samples/exhausted-quota-manifest.json
    --samples ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tile_samples/valid.tiles.jsonl)
foreach(bad_tile_sample
    bad-class
    bad-tileop
    duplicate
    out-of-grid)
  add_test(NAME tile_sample_fixture_${bad_tile_sample}_reject
    COMMAND tile_sample_check
      --manifest ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tile_samples/valid-manifest.json
      --samples ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tile_samples/${bad_tile_sample}.tiles.jsonl)
  set_tests_properties(tile_sample_fixture_${bad_tile_sample}_reject PROPERTIES WILL_FAIL TRUE)
endforeach()
add_test(NAME tile_sample_fixture_under_quota_reject
  COMMAND tile_sample_check
    --manifest ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tile_samples/under-quota-manifest.json
    --samples ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tile_samples/valid.tiles.jsonl)
set_tests_properties(tile_sample_fixture_under_quota_reject PROPERTIES WILL_FAIL TRUE)

foreach(postflight_case
    run-contract-pass:RUN_CONTRACT_PASS
    analytical-telemetry-pass:RUN_CONTRACT_PASS
    tile-sample-audit-pass:TILE_SAMPLE_AUDIT_PASS
    span-proof-pass:SPAN_PROOF_PASS
    moat-claim-proof-missing:CLAIM_PROOF_MISSING)
  string(REPLACE ":" ";" postflight_parts "${postflight_case}")
  list(GET postflight_parts 0 postflight_name)
  list(GET postflight_parts 1 postflight_status)
  add_test(NAME postflight_fixture_${postflight_name}
    COMMAND postflight_check
      ${CMAKE_CURRENT_SOURCE_DIR}/postflight/fixtures/${postflight_name}.json)
  set_tests_properties(postflight_fixture_${postflight_name}
    PROPERTIES PASS_REGULAR_EXPRESSION "\"status\":\"${postflight_status}\"|\"status\": \"${postflight_status}\"")
endforeach()
add_test(NAME postflight_fixture_reject_overflow
  COMMAND postflight_check ${CMAKE_CURRENT_SOURCE_DIR}/postflight/fixtures/reject-overflow.json)
set_tests_properties(postflight_fixture_reject_overflow PROPERTIES WILL_FAIL TRUE)
add_test(NAME postflight_fixture_reject_analytical_telemetry
  COMMAND postflight_check ${CMAKE_CURRENT_SOURCE_DIR}/postflight/fixtures/reject-analytical-telemetry.json)
set_tests_properties(postflight_fixture_reject_analytical_telemetry PROPERTIES WILL_FAIL TRUE)
