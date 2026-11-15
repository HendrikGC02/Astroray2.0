// SPDX-License-Identifier: Apache-2.0
// Test-only module (never embedded): evaluates sample() and sample_independent() of the
// production sampler for host-generated keys, so the test can compare the bits with the
// CPU build of the same functions. Ref: 0003 d23, check 24; 0009 d17(i).
#include "sample/sobol.h"

struct SamplerTuple {
  uint32_t id, index, stream, d, seed;
};

// out[2i] and out[2i + 1] are the float bits of sample() and sample_independent() for
// tuple i; bits, not values, so the comparison is exact.
extern "C" __global__ void ar_sampler_agree(const SamplerTuple* tuples, uint32_t* out, uint32_t n) {
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const SamplerTuple t = tuples[i];
  out[2u * i] = ar::bit_cast<uint32_t>(ar::sample(t.id, t.index, t.stream, t.d, t.seed));
  out[2u * i + 1u] = ar::bit_cast<uint32_t>(ar::sample_independent(t.id, t.index, t.stream, t.d, t.seed));
}
