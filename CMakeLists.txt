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
set(TARGET_DEVICE "jetson" CACHE STRING "Target device: jetson or a100")

if(TARGET_DEVICE STREQUAL "a100")
    message(STATUS "Target device: A100 (SM 8.0, 108 SMs)")
    set(CMAKE_CUDA_ARCHITECTURES 80)
    set(GM_MAXRREGCOUNT 64)
    add_compile_definitions(TARGET_A100)
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
