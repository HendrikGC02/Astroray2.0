// SPDX-License-Identifier: Apache-2.0
// Test-only module (never embedded): the smallest kernel that proves the device build rules
// and the module loader. It runs the portability header's CUDA branch (load, math, atomics,
// warp helpers) so a header nvcc rejects fails this build, not a stage. Ref: 0002 D14.
#include "portability.h"

// out[i] = sqrt(in[i]) + 1; lanes[i] = the thread's lane; *counter counts the threads that ran;
// *ballot_or collects the warp ballots.
extern "C" __global__ void ar_selftest(const float* in, float* out, uint32_t* lanes, uint32_t* counter,
                                       uint32_t* ballot_or, uint32_t n) {
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t votes = ar::ballot(i < n);
  if (i >= n) return;
  out[i] = ar::sqrt(ar::load_ro(in + i)) + 1.0f;
  lanes[i] = ar::lane_id();
  ar::atomic_add(counter, 1u);
  ar::atomic_max(ballot_or, votes);
}
