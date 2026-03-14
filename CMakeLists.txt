cmake_minimum_required(VERSION 3.18)
project(gaussian_moat_cuda LANGUAGES CUDA CXX)

set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Jetson Orin Nano — Ampere SM 8.7
set(CMAKE_CUDA_ARCHITECTURES 87)

# Compiler flags
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -O3 --use_fast_math -lineinfo --ptxas-options=-v")

# --- Main target ---
add_executable(gm_cuda_primes
    src/main.cu
    src/kernel.cu
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
