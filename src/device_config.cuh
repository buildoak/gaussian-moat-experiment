#ifndef GM_DEVICE_CONFIG_CUH
#define GM_DEVICE_CONFIG_CUH

// Device-agnostic configuration for Gaussian moat CUDA sieve.
//
// Compile with -DTARGET_A100 for A100 tuning, otherwise defaults to
// Jetson Orin Nano parameters.
//
// Usage in CMake:
//   cmake -DTARGET_DEVICE=a100 ..   → adds -DTARGET_A100
//   cmake -DTARGET_DEVICE=jetson .. → default (no define)

#if defined(TARGET_A100)
  // NVIDIA A100: SM 8.0, 108 SMs, 6912 CUDA cores, 80GB HBM2e
  #define GRID_CAP                4096
  #define DEVICE_SEGMENT_SPAN     (1u << 19)   // 512K norms per segment, 32KB shared mem
  #define DEVICE_BITMAP_WORDS     8192u         // 512K/2 = 256K odd slots, /32 = 8192 words
  #define DEVICE_BITMAP_BYTES     (DEVICE_BITMAP_WORDS * 4u)
  #define MAX_REG_COUNT           64
  #define THERMAL_CHECK_ENABLED   0
  #define USE_MANAGED_MEMORY      0
  #define SEGMENTS_PER_BATCH_BASE 8000
#else
  // Jetson Orin Nano: SM 8.7, 8 SMs, 1024 CUDA cores, 8GB unified mem
  #define GRID_CAP                1024
  #define DEVICE_SEGMENT_SPAN     (1u << 18)   // 256K norms per segment, 16KB shared mem
  #define DEVICE_BITMAP_WORDS     4096u         // 256K/2 = 128K odd slots, /32 = 4096 words
  #define DEVICE_BITMAP_BYTES     (DEVICE_BITMAP_WORDS * 4u)
  #define MAX_REG_COUNT           48
  #define THERMAL_CHECK_ENABLED   1
  #define USE_MANAGED_MEMORY      1
  #define SEGMENTS_PER_BATCH_BASE 2000
#endif

#endif // GM_DEVICE_CONFIG_CUH
