cmake_minimum_required(VERSION 3.18)
project(gaussian_moat_cuda LANGUAGES CUDA CXX)

set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Device selection ---
# Usage:
#   cmake -DTARGET_DEVICE=jetson ..   (default — Jetson Orin Nano, SM 8.7)
#   cmake -DTARGET_DEVICE=a100 ..     (A100, SM 8.0)
#   cmake -DTARGET_DEVICE=3090 ..     (RTX 3090, SM 8.6 Ampere)
#   cmake -DTARGET_DEVICE=4090 ..     (RTX 4090, SM 8.9 Ada Lovelace)
set(TARGET_DEVICE "jetson" CACHE STRING "Target device: jetson, a100, 3090, or 4090")

if(TARGET_DEVICE STREQUAL "a100")
    message(STATUS "Target device: A100 (SM 8.0, 108 SMs)")
    set(CMAKE_CUDA_ARCHITECTURES 80)
    set(GM_MAXRREGCOUNT 64)
    add_compile_definitions(TARGET_A100)
elseif(TARGET_DEVICE STREQUAL "3090")
    message(STATUS "Target device: RTX 3090 (SM 8.6, 82 SMs, Ampere)")
    set(CMAKE_CUDA_ARCHITECTURES 86)
    set(GM_MAXRREGCOUNT 64)
    add_compile_definitions(TARGET_3090)
elseif(TARGET_DEVICE STREQUAL "4090")
    message(STATUS "Target device: RTX 4090 (SM 8.9, 128 SMs, Ada Lovelace)")
    set(CMAKE_CUDA_ARCHITECTURES 89)
    set(GM_MAXRREGCOUNT 64)
    add_compile_definitions(TARGET_4090)
else()
    message(STATUS "Target device: Jetson Orin Nano (SM 8.7, 8 SMs)")
    set(CMAKE_CUDA_ARCHITECTURES 87)
    set(GM_MAXRREGCOUNT 48)
endif()

# Compiler flags
# -O3: maximum optimization
# --use_fast_math: faster transcendentals (used in isqrt seed)
# -lineinfo: debug info without perf cost
# --ptxas-options=-v: register/smem usage reporting
# --maxrregcount: tuned per device (48 Jetson, 64 A100)
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -O3 --use_fast_math -lineinfo --ptxas-options=-v --maxrregcount=${GM_MAXRREGCOUNT}")

# --- Main target ---
add_executable(gm_cuda_primes
    src/main.cu
    src/kernel.cu
    src/sieve_kernel.cu
)
target_include_directories(gm_cuda_primes PRIVATE src/)

# --- Test targets ---
add_executable(gm_cuda_tests
    tests/test_modular.cu
)
target_include_directories(gm_cuda_tests PRIVATE src/)

add_executable(gm_test_miller_rabin
    tests/test_miller_rabin.cu
)
target_include_directories(gm_test_miller_rabin PRIVATE src/)

add_executable(gm_test_cornacchia
    tests/test_cornacchia.cu
)
target_include_directories(gm_test_cornacchia PRIVATE src/)

add_executable(gm_test_integration
    tests/test_integration.cu
)
target_include_directories(gm_test_integration PRIVATE src/)

# --- Sieve test target ---
add_executable(gm_test_sieve
    tests/test_sieve.cu
    src/sieve_kernel.cu
)
target_include_directories(gm_test_sieve PRIVATE src/)

add_executable(tile_cuda
    src/tile_main.cu
)
target_include_directories(tile_cuda PRIVATE src/)
