# tests/CMakeLists.txt
#
# One GoogleTest executable per component. gtest_discover_tests registers
# each case with CTest for parallel `ctest` output.

include(GoogleTest)

function(campaign_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE campaign GTest::gtest_main)
  gtest_discover_tests(${name})
endfunction()

campaign_add_test(test_grid)
campaign_add_test(test_region)
campaign_add_test(test_campaign_constants)
campaign_add_test(test_primality)
campaign_add_test(test_sieve)
campaign_add_test(test_union_find)
campaign_add_test(test_geo_tests)
campaign_add_test(test_tileop)
campaign_add_test(test_compositor)
campaign_add_test(test_snapshot)
target_compile_definitions(test_snapshot PRIVATE
  COMPARE_SNAPSHOTS_PATH="$<TARGET_FILE:compare_snapshots>")
campaign_add_test(test_golden_5tile)
