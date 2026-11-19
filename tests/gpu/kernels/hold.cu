// SPDX-License-Identifier: Apache-2.0
// Test-only module (never embedded): one thread that keeps its stream busy for `ns` nanoseconds of
// wall time, so a test can queue host-staged copies behind it and know none has run when the host
// moves on. The time comes from the GPU's global nanosecond timer.
// Ref: PTX ISA, special register %globaltimer; CUDA C++ Programming Guide, __nanosleep.
#include <cstdint>

extern "C" __global__ void ar_test_hold(uint64_t ns) {
  uint64_t t0 = 0, t = 0;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t0));
  do {
    __nanosleep(1000);
    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t));
  } while (t - t0 < ns);
}
