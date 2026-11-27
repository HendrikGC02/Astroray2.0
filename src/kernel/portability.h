// SPDX-License-Identifier: Apache-2.0
// The only kernel file that knows its target (nvcc or MSVC). Everything else in
// src/kernel/ calls the names below. No include appears outside this file.
// Ref: docs/decisions/0002-kernel-architecture.md D1, D2, D12. Patterns only:
// Cycles kernel/device/cuda/compat.h and pbrt-v4 PBRT_CPU_GPU (both Apache-2.0).
#pragma once
#include <cstdint>

// The host compiler's spelling of noinline. nvcc honors it on a device function (ptxas lists the
// function on its own); the kernel lint bans the CUDA spelling, __noinline__.
#if defined(_MSC_VER)
#define AR_NOINLINE_ATTR __declspec(noinline)
#elif defined(__GNUC__)
#define AR_NOINLINE_ATTR __attribute__((noinline))
#else
#error "portability.h: no noinline spelling for this compiler"
#endif

#if defined(__CUDACC__)
#define AR_CUDA_BUILD 1
#define AR_FN __host__ __device__ __forceinline__
// A function kept out of line: a rare tail that would otherwise sit in a hot kernel's live range.
#define AR_NOINLINE __host__ __device__ AR_NOINLINE_ATTR inline
#else
#define AR_CUDA_BUILD 0
#include <cmath>
#include <intrin.h>
#define AR_FN inline
#define AR_NOINLINE AR_NOINLINE_ATTR inline
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

// Population count, for the ranks of a warp vote. Explicit on both targets, so the GPU's one
// POPC does not depend on nvcc recognising a bit loop. MSVC's __popcnt emits POPCNT, which
// every AVX2 CPU has; the CPU device requires AVX2 (0001 D3.6, 0002 D12).
// Ref: CUDA Math API, integer intrinsics (__popc); Microsoft C++ docs, __popcnt16/__popcnt/__popcnt64.
AR_FN uint32_t popc(uint32_t x) {
#if AR_CUDA_BUILD
  return static_cast<uint32_t>(__popc(x));
#else
  return __popcnt(x);
#endif
}

// The lanes of the warp holding the same value as this lane, one bit per lane: the ranks of a
// stable scatter with many bins, where a ballot per bin costs more than it saves. The CPU is one
// lane. Ref: CUDA C++ Programming Guide, warp match functions (__match_any_sync, sm_70 and later).
AR_FN uint32_t match_any(uint32_t v) {
#if AR_CUDA_BUILD
  return __match_any_sync(0xffffffffu, v);
#else
  (void)v;
  return 1u;
#endif
}

// A barrier with memory ordering among the lanes of a warp, for lanes that read what other lanes
// of the same warp wrote; nothing on the CPU (one lane). Ref: CUDA C++ Programming Guide, __syncwarp
// (the other *_sync intrinsics converge lanes but promise no memory ordering).
AR_FN void sync_lanes() {
#if AR_CUDA_BUILD
  __syncwarp();
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

// Math wrappers overloaded on float and double: the CUDA library functions on the GPU (no fast
// math, D12), <cmath> on the CPU. Only sqrt (built with -prec-sqrt=true), fma, floor, ceil and
// abs are exact or correctly rounded on both targets. The others (exp, expm1, log, the
// trigonometric ones, atan2, cosh, pow) carry library-specific ulp errors; powf in particular
// differs from MSVC's in the last bit for some inputs, so bit-exact CPU/GPU code must not rely
// on them. Ref: CUDA C++ Programming Guide, appendix "Mathematical Functions" (ulp error tables).
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

// sin(pi x) and cos(pi x), for angles that are pi times a sampled value (phi = 2 pi u). CUDA's
// sinpif / cospif reduce the argument exactly, so they carry no double-precision slow path, which
// sinf / cosf do and which a GPU shade kernel may not contain (0002 D8, ptxas
// --warn-on-double-precision-use). MSVC's C++17 library has no sinpi, so the CPU evaluates
// sin(pi x) in double and rounds once; both stay within about 1 ulp of the true value.
// Ref: CUDA C++ Programming Guide, appendix "Mathematical Functions" (sinpif, cospif: 1 ulp).
inline constexpr double kPiDouble = double(3.14159265358979323846);  // pi to double precision
#if AR_CUDA_BUILD
AR_FN float sinpi(float x) { return ::sinpif(x); }
AR_FN double sinpi(double x) { return ::sinpi(x); }
AR_FN float cospi(float x) { return ::cospif(x); }
AR_FN double cospi(double x) { return ::cospi(x); }
#else
AR_FN float sinpi(float x) { return float(std::sin(kPiDouble * double(x))); }
AR_FN double sinpi(double x) { return std::sin(kPiDouble * x); }
AR_FN float cospi(float x) { return float(std::cos(kPiDouble * double(x))); }
AR_FN double cospi(double x) { return std::cos(kPiDouble * x); }
#endif

AR_FN float fma(float x, float y, float z) { return AR_MATH_F(fma, x, y, z); }
AR_FN double fma(double x, double y, double z) { return AR_MATH_D(fma, x, y, z); }

// Plain comparisons on both targets so NaN handling is identical.
AR_FN float min(float a, float b) { return b < a ? b : a; }
AR_FN double min(double a, double b) { return b < a ? b : a; }
AR_FN float max(float a, float b) { return a < b ? b : a; }
AR_FN double max(double a, double b) { return a < b ? b : a; }

// One 8-bit texel as RGBA floats, raw: value / 255 with no transfer function (filtering happens
// on stored values, 0005 item 34). Grey expands to (g, g, g, 1) and RGB gets alpha 1, as Cycles
// reads single-channel bytes and stores RGB bytes as RGBA. `p` is the texel's first byte
// (image_texel in scene/texture_view.h); the bytes go through the read-only path (0002 D13).
// Ref: Cycles intern/cycles/kernel/util/image.h, ImageInterpolator::read for uchar and uchar4
// (r * (1.0f / 255.0f)), and kernel_image_interp (make_float4(f, f, f, 1.0f)) (Apache-2.0).
struct Texel {
  float v[4];
};

AR_FN Texel load_texel(const uint8_t* p, uint32_t channels) {
  const float f = 1.0f / 255.0f;
  Texel t;
  if (channels == 1u) {
    const float g = float(load_ro(p)) * f;
    t.v[0] = t.v[1] = t.v[2] = g;
    t.v[3] = 1.0f;
    return t;
  }
  for (uint32_t c = 0; c < 3u; ++c) t.v[c] = float(load_ro(p + c)) * f;
  t.v[3] = channels == 4u ? float(load_ro(p + 3)) * f : 1.0f;
  return t;
}

#undef AR_MATH1
#undef AR_MATH2
#undef AR_MATH_F
#undef AR_MATH_D

}  // namespace ar
