// SPDX-License-Identifier: Apache-2.0
// Gate-hook builds only (never embedded, never in Release): the out-of-bounds kernel write of 0001 V14.
// CudaDevice runs it once, before a device's first launch, when ASTRORAY_TEST_GPU_FAULT=1. The write
// lands `index` words past `base`, far outside any allocation, so the driver reports an illegal address
// at the next event wait: a sticky CUDA error, the process's GPU is dead afterwards (0001 D2.5).
// Ref: docs/decisions/0001-product-shape.md D2.5, V14; 0002 D15; CUDA Driver API (CUDA_ERROR_ILLEGAL_ADDRESS).
#include "launch_config.h"
#include "portability.h"

extern "C" __global__ void AR_LAUNCH_BOUNDS(ar::kLaunch_ar_test_fault.threads, ar::kLaunch_ar_test_fault.min_blocks)
    ar_test_fault(uint32_t* base, uint64_t index) {
  // volatile: the store must not be removed as dead.
  *reinterpret_cast<volatile uint32_t*>(base + index) = 0xDEADBEEFu;
}
