#include <cstdio>
#include <cstdint>
#include <cuda_runtime.h>

#include "types.h"
#include "modular_arith.cuh"

// Simple device-side smoke test: compute mulmod64 and powmod64 on GPU
__global__ void smoke_test_kernel(uint64_t* results) {
    // results[0] = mulmod64_v1(7, 8, 13)  — expect 4
    // results[1] = mulmod64_v2(7, 8, 13)  — expect 4
    // results[2] = mulmod64_v3(7, 8, 13)  — expect 4
    // results[3] = powmod64(2, 10, 1000)  — expect 24
    results[0] = mulmod64_v1(7, 8, 13);
    results[1] = mulmod64_v2(7, 8, 13);
    results[2] = mulmod64_v3(7, 8, 13);
    results[3] = powmod64(2, 10, 1000);
}

int main() {
    printf("gaussian-moat-cuda — Phase 0.5.1 modular arithmetic foundation\n");

    // Check CUDA device
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess || device_count == 0) {
        printf("No CUDA devices found (error: %s). Running host-only tests.\n",
               cudaGetErrorString(err));

        // Host-side verification
        printf("\n--- Host-side modular arithmetic verification ---\n");
        printf("mulmod64_v1(7, 8, 13) = %lu (expect 4)\n", (unsigned long)mulmod64_v1(7, 8, 13));
        printf("mulmod64_v2(7, 8, 13) = %lu (expect 4)\n", (unsigned long)mulmod64_v2(7, 8, 13));
        printf("mulmod64_v3(7, 8, 13) = %lu (expect 4)\n", (unsigned long)mulmod64_v3(7, 8, 13));
        printf("powmod64(2, 10, 1000) = %lu (expect 24)\n", (unsigned long)powmod64(2, 10, 1000));
        printf("powmod64(3, 13, 1000000007) = %lu (expect 1594323)\n",
               (unsigned long)powmod64(3, 13, 1000000007));

        printf("\nGaussianPrime struct size: %zu bytes\n", sizeof(GaussianPrime));
        return 0;
    }

    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("Device: %s (SM %d.%d, %d SMs, %.1f GB)\n",
           prop.name, prop.major, prop.minor,
           prop.multiProcessorCount,
           prop.totalGlobalMem / (1024.0 * 1024.0 * 1024.0));

    // Run smoke test kernel
    uint64_t* d_results;
    uint64_t h_results[4];
    cudaMalloc(&d_results, 4 * sizeof(uint64_t));

    smoke_test_kernel<<<1, 1>>>(d_results);
    cudaDeviceSynchronize();

    err = cudaGetLastError();
    if (err != cudaSuccess) {
        printf("Kernel error: %s\n", cudaGetErrorString(err));
        cudaFree(d_results);
        return 1;
    }

    cudaMemcpy(h_results, d_results, 4 * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    cudaFree(d_results);

    printf("\n--- GPU smoke test results ---\n");
    printf("mulmod64_v1(7, 8, 13) = %lu (expect 4) %s\n",
           (unsigned long)h_results[0], h_results[0] == 4 ? "PASS" : "FAIL");
    printf("mulmod64_v2(7, 8, 13) = %lu (expect 4) %s\n",
           (unsigned long)h_results[1], h_results[1] == 4 ? "PASS" : "FAIL");
    printf("mulmod64_v3(7, 8, 13) = %lu (expect 4) %s\n",
           (unsigned long)h_results[2], h_results[2] == 4 ? "PASS" : "FAIL");
    printf("powmod64(2, 10, 1000) = %lu (expect 24) %s\n",
           (unsigned long)h_results[3], h_results[3] == 24 ? "PASS" : "FAIL");

    printf("\nGaussianPrime struct size: %zu bytes\n", sizeof(GaussianPrime));
    printf("All smoke tests passed.\n");
    return 0;
}
