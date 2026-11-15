// SPDX-License-Identifier: Apache-2.0
// The only kernel file that knows its target (nvcc or MSVC). Everything else in
// src/kernel/ calls the names below. No include appears outside this file.
// Ref: docs/decisions/0002-kernel-architecture.md D1, D2, D12. Patterns only:
// Cycles kernel/device/cuda/compat.h and pbrt-v4 PBRT_CPU_GPU (both Apache-2.0).
#pragma once
#include <cstdint>

#if defined(__CUDACC__)
#define AR_CUDA_BUILD 1
#define AR_FN __host__ __device__ __forceinline__
#else
#define AR_CUDA_BUILD 0
#include <cmath>
#include <intrin.h>
#define AR_FN inline
#endif

// Asserts exist only in AR_DEBUG builds; a release kernel never traps.
#if defined(AR_DEBUG)
#if AR_CUDA_BUILD
#define AR_ASSERT(cond) ((cond) ? void(0) : __trap())
#else
#define AR_ASSERT(cond) ((cond) ? void(0) : __debugbreak())
#endif
#else
#define AR_ASSERT(cond) void(0)
#endif

namespace ar {

inline constexpr bool AR_GPU = AR_CUDA_BUILD != 0;

// Read-only load: the texture-cache path on the GPU, a plain load on the CPU.
template <class T>
AR_FN T load_ro(const T* p) {
#if AR_CUDA_BUILD
  return __ldg(p);
#else
  return *p;
#endif
}

template <class To, class From>
AR_FN To bit_cast(const From& from) {
  static_assert(sizeof(To) == sizeof(From), "bit_cast needs equal sizes");
  return __builtin_bit_cast(To, from);
}

// Atomics return the previous value, as CUDA's do. The CPU versions are
// sequentially consistent (MSVC interlocked intrinsics).
AR_FN uint32_t atomic_cas(uint32_t* p, uint32_t expected, uint32_t desired) {
#if AR_CUDA_BUILD
  return atomicCAS(p, expected, desired);
#else
  return static_cast<uint32_t>(_InterlockedCompareExchange(
      reinterpret_cast<volatile long*>(p), static_cast<long>(desired), static_cast<long>(expected)));
#endif
}

AR_FN uint32_t atomic_add(uint32_t* p, uint32_t v) {
#if AR_CUDA_BUILD
  return atomicAdd(p, v);
#else
  return static_cast<uint32_t>(
      _InterlockedExchangeAdd(reinterpret_cast<volatile long*>(p), static_cast<long>(v)));
#endif
}

AR_FN float atomic_add(float* p, float v) {
#if AR_CUDA_BUILD
  return atomicAdd(p, v);
#else
  uint32_t* bits = reinterpret_cast<uint32_t*>(p);
  uint32_t old = *reinterpret_cast<volatile uint32_t*>(bits);
  for (;;) {
    const uint32_t seen = atomic_cas(bits, old, bit_cast<uint32_t>(bit_cast<float>(old) + v));
    if (seen == old) return bit_cast<float>(old);
    old = seen;
  }
#endif
}

AR_FN uint32_t atomic_max(uint32_t* p, uint32_t v) {
#if AR_CUDA_BUILD
  return atomicMax(p, v);
#else
  uint32_t old = *reinterpret_cast<volatile uint32_t*>(p);
  while (old < v) {
    const uint32_t seen = atomic_cas(p, old, v);
    if (seen == old) break;
    old = seen;
  }
  return old;
#endif
}

AR_FN uint64_t clock() {
#if AR_CUDA_BUILD
  return static_cast<uint64_t>(clock64());
#else
  return __rdtsc();
#endif
}

// Warp helpers; the CPU is a single lane.
AR_FN uint32_t ballot(bool pred) {
#if AR_CUDA_BUILD
  return __ballot_sync(0xffffffffu, pred);
#else
  return pred ? 1u : 0u;
#endif
}

AR_FN uint32_t lane_id() {
#if AR_CUDA_BUILD
  uint32_t id;
  asm volatile("mov.u32 %0, %%laneid;" : "=r"(id));
  return id;
#else
  return 0u;
#endif
}

// Math wrappers overloaded on float and double. On the GPU they are the
// correctly rounded CUDA functions (no fast math, D12); on the CPU <cmath>.
#if AR_CUDA_BUILD
#define AR_MATH_F(c, ...) ::c##f(__VA_ARGS__)
#define AR_MATH_D(c, ...) ::c(__VA_ARGS__)
#else
#define AR_MATH_F(c, ...) std::c(__VA_ARGS__)
#define AR_MATH_D(c, ...) std::c(__VA_ARGS__)
#endif
#define AR_MATH1(name, c)                                          \
  AR_FN float name(float x) { return AR_MATH_F(c, x); }            \
  AR_FN double name(double x) { return AR_MATH_D(c, x); }
#define AR_MATH2(name, c)                                                      \
  AR_FN float name(float x, float y) { return AR_MATH_F(c, x, y); }            \
  AR_FN double name(double x, double y) { return AR_MATH_D(c, x, y); }

AR_MATH1(sqrt, sqrt)
AR_MATH1(exp, exp)
AR_MATH1(expm1, expm1)
AR_MATH1(log, log)
AR_MATH1(sin, sin)
AR_MATH1(cos, cos)
AR_MATH1(tan, tan)
AR_MATH1(acos, acos)
AR_MATH1(asin, asin)
AR_MATH1(floor, floor)
AR_MATH1(ceil, ceil)
AR_MATH1(cosh, cosh)
AR_MATH1(abs, fabs)
AR_MATH2(atan2, atan2)
AR_MATH2(pow, pow)

AR_FN float fma(float x, float y, float z) { return AR_MATH_F(fma, x, y, z); }
AR_FN double fma(double x, double y, double z) { return AR_MATH_D(fma, x, y, z); }

// Plain comparisons on both targets so NaN handling is identical.
AR_FN float min(float a, float b) { return b < a ? b : a; }
AR_FN double min(double a, double b) { return b < a ? b : a; }
AR_FN float max(float a, float b) { return a < b ? b : a; }
AR_FN double max(double a, double b) { return a < b ? b : a; }

#undef AR_MATH1
#undef AR_MATH2
#undef AR_MATH_F
#undef AR_MATH_D

}  // namespace ar
