cmake_minimum_required(VERSION 3.22)
project(cpp_campaign_v2 CXX)

# ---------------------------------------------------------------------------
# Cache vars
# ---------------------------------------------------------------------------
if(NOT DEFINED K_SQ)
  message(FATAL_ERROR
    "K_SQ must be set via -DK_SQ=36 or -DK_SQ=40 (execution plan §9)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CAMPAIGN_STRICT
  "Inject #pragma GCC poison sqrt sqrtf sqrtl into sieve.h via CAMPAIGN_STRICT"
  ON)
option(CAMPAIGN_BUILD_TESTS
  "Build GoogleTest-based unit tests"
  ON)

# ---------------------------------------------------------------------------
# Compile options
# ---------------------------------------------------------------------------
# Note: -Wpedantic removed because __int128 is a valid GCC extension required
# for 128-bit arithmetic in geo_tests.cpp and campaign_constants.cpp
add_compile_options(-Wall -Wextra -Werror)

# Protect against the Apple/Clang libc++ which inlines sqrt templates in
# <cmath>. If CAMPAIGN_STRICT + the pragma poisons `sqrt`, transitive
# includes from <algorithm> or <limits> via GoogleTest would fail. The
# pragma is scoped to sieve.h only; CAMPAIGN_STRICT is expressed as a
# macro define consumed there.
if(CAMPAIGN_STRICT)
  add_compile_definitions(CAMPAIGN_STRICT=1)
endif()

# ---------------------------------------------------------------------------
# Dependencies — FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# GoogleTest pinned to a release tag (plan §3 / Q6 resolution).
if(CAMPAIGN_BUILD_TESTS)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

# nlohmann/json pinned (plan §3 / Q7 resolution).
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# OpenMP for the per-tile parallel loop (Q2 resolution).
# Apple's AppleClang ships without OpenMP by default. We REQUIRE it so
# builds fail loudly when it's missing — reinstall via Homebrew libomp.
find_package(OpenMP REQUIRED)

# ---------------------------------------------------------------------------
# Library: campaign
# ---------------------------------------------------------------------------
set(CAMPAIGN_SRC
  src/sha256.cpp
  src/campaign_constants.cpp
  src/primality.cpp
  src/region.cpp
  src/grid.cpp
  src/sieve.cpp
  src/union_find.cpp
  src/geo_tests.cpp
  src/tileop.cpp
  src/compositor.cpp
  src/snapshot.cpp
  src/process_tile.cpp
)

add_library(campaign STATIC ${CAMPAIGN_SRC})

target_include_directories(campaign
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(campaign PUBLIC K_SQ=${K_SQ})

target_link_libraries(campaign
  PUBLIC
    nlohmann_json::nlohmann_json
    OpenMP::OpenMP_CXX
)

# ---------------------------------------------------------------------------
# Pre-build BZ check target
# ---------------------------------------------------------------------------
# Audit Codex-M2 (2026-04-21): radii are sourced from scripts/bz_config.json
# (single source of truth) so CMake and bz_check.py cannot drift. Tiny-radius
# unit tests do NOT trigger this check.

# bz_check.py is a PEP 723 script: it declares its own `mpmath` dependency
# inline. Prefer `uv run` (which understands the inline dep block and
# materialises an isolated venv). Fall back to plain python3 when uv is
# absent — CI installs mpmath explicitly in that case.
find_program(UV_EXECUTABLE uv)
find_package(Python3 COMPONENTS Interpreter)

if(UV_EXECUTABLE)
  set(BZ_CHECK_CMD ${UV_EXECUTABLE} run --script
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/bz_check.py)
elseif(Python3_Interpreter_FOUND)
  set(BZ_CHECK_CMD ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/bz_check.py)
else()
  set(BZ_CHECK_CMD "")
endif()

if(BZ_CHECK_CMD)
  # bz_check.py loads R_inner/R_outer for K_SQ from scripts/bz_config.json
  # when not supplied on the CLI. The target depends on the config file so
  # edits re-trigger the gate.
  add_custom_target(bz_check ALL
    COMMAND ${BZ_CHECK_CMD} --k-sq ${K_SQ}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/bz_config.json
    COMMENT "Running bz_check.py (BZ reconciliation, blueprint §2)"
    VERBATIM
  )
  add_dependencies(campaign bz_check)
else()
  message(WARNING
    "Neither uv nor Python3 found — skipping bz_check target. Blueprint "
    "§2 gate will be enforced in CI.")
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(campaign_main apps/campaign_main.cpp)
target_link_libraries(campaign_main PRIVATE campaign OpenMP::OpenMP_CXX)

add_executable(compare_snapshots apps/compare_snapshots.cpp)
target_link_libraries(compare_snapshots PRIVATE campaign)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(CAMPAIGN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
