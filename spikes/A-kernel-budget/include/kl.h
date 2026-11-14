// Spike A: restricted-C++ kernel library, core types. Throwaway measurement code.
// No virtuals, no exceptions, no STL. Compiled by MSVC (CPU) and nvcc (GPU).
#pragma once
#ifdef __CUDACC_RTC__
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
#else
#include <stdint.h>
#include <math.h>
#endif

#ifdef __CUDACC__
#define KL_HD __host__ __device__
#define KL_FORCEINLINE __forceinline__
#define KL_NOINLINE_ATTR __noinline__
#else
#define KL_HD
#define KL_FORCEINLINE inline
#define KL_NOINLINE_ATTR
#endif

#ifdef KL_REAL_DOUBLE
typedef double Real;
#else
typedef float Real;
#endif

// Lobe functions are the units the old engine could not keep out of line.
// KL_NOINLINE_LOBES=1 marks them __noinline__ (GPU only; CPU build ignores it).
#if defined(KL_NOINLINE_LOBES) && KL_NOINLINE_LOBES && defined(__CUDA_ARCH__)
#define LOBE_FN KL_HD KL_NOINLINE_ATTR
#else
#define LOBE_FN KL_HD KL_FORCEINLINE
#endif
#define FN KL_HD KL_FORCEINLINE

#ifndef KL_WIDTH
#define KL_WIDTH 4
#endif

// Material record passed to lobe functions by value or by pointer (KL_MAT_BYPTR).
#if defined(KL_MAT_BYPTR) && KL_MAT_BYPTR
#define MAT_ARG(T) const T* mp_
#define MAT_USE(T) const T& m = *mp_
#define MAT_PASS(x) (&(x))
#else
#define MAT_ARG(T) T mp_
#define MAT_USE(T) const T& m = mp_
#define MAT_PASS(x) (x)
#endif

namespace kl {
static constexpr Real PI = (Real)3.14159265358979323846;
static constexpr Real INV_PI = (Real)0.31830988618379067154;
FN Real sqrt_(Real x) { return (Real)sqrt(x); }
FN Real safe_sqrt(Real x) { return sqrt_(x > (Real)0 ? x : (Real)0); }
FN Real abs_(Real x) { return x < 0 ? -x : x; }
FN Real min_(Real a, Real b) { return a < b ? a : b; }
FN Real max_(Real a, Real b) { return a > b ? a : b; }
FN Real clamp_(Real x, Real a, Real b) { return x < a ? a : (x > b ? b : x); }
FN Real sqr(Real x) { return x * x; }
FN Real lerp_(Real t, Real a, Real b) { return a + t * (b - a); }
FN Real sin_(Real x) { return (Real)sin(x); }
FN Real cos_(Real x) { return (Real)cos(x); }
FN Real exp_(Real x) { return (Real)exp(x); }
FN Real exp2_(Real x) { return (Real)exp2(x); }
FN Real log_(Real x) { return (Real)log(x); }
FN Real pow_(Real a, Real b) { return (Real)pow(a, b); }
FN Real atan2_(Real y, Real x) { return (Real)atan2(y, x); }

struct V3 { Real x, y, z; };
FN V3 v3(Real x, Real y, Real z) { V3 r; r.x = x; r.y = y; r.z = z; return r; }
FN V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
FN V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
FN V3 operator-(V3 a) { return v3(-a.x, -a.y, -a.z); }
FN V3 operator*(V3 a, Real s) { return v3(a.x * s, a.y * s, a.z * s); }
FN V3 operator*(Real s, V3 a) { return v3(a.x * s, a.y * s, a.z * s); }
FN Real dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
FN V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
FN Real length2(V3 a) { return dot(a, a); }
FN V3 normalize(V3 a) { Real l = sqrt_(dot(a, a)); Real il = (Real)1 / l; return a * il; }
FN V3 face_forward(V3 n, V3 v) { return dot(n, v) < 0 ? -n : n; }

struct Frame { V3 t, b, n; };
FN V3 to_local(const Frame& f, V3 v) { return v3(dot(v, f.t), dot(v, f.b), dot(v, f.n)); }
FN V3 to_world(const Frame& f, V3 v) { return f.t * v.x + f.b * v.y + f.n * v.z; }

// Spectral packet: N wavelengths processed together (hero-wavelength style).
template <int N> struct Spec { Real v[N]; };
template <int N> FN Spec<N> sset(Real a) { Spec<N> r; for (int i = 0; i < N; ++i) r.v[i] = a; return r; }
template <int N> FN Spec<N> operator+(const Spec<N>& a, const Spec<N>& b) { Spec<N> r; for (int i = 0; i < N; ++i) r.v[i] = a.v[i] + b.v[i]; return r; }
template <int N> FN Spec<N> operator-(const Spec<N>& a, const Spec<N>& b) { Spec<N> r; for (int i = 0; i < N; ++i) r.v[i] = a.v[i] - b.v[i]; return r; }
template <int N> FN Spec<N> operator*(const Spec<N>& a, const Spec<N>& b) { Spec<N> r; for (int i = 0; i < N; ++i) r.v[i] = a.v[i] * b.v[i]; return r; }
template <int N> FN Spec<N> operator*(const Spec<N>& a, Real s) { Spec<N> r; for (int i = 0; i < N; ++i) r.v[i] = a.v[i] * s; return r; }
template <int N> FN Spec<N> operator*(Real s, const Spec<N>& a) { return a * s; }
template <int N> FN Real savg(const Spec<N>& a) { Real s = 0; for (int i = 0; i < N; ++i) s += a.v[i]; return s * ((Real)1 / (Real)N); }
template <int N> FN Real smax(const Spec<N>& a) { Real s = a.v[0]; for (int i = 1; i < N; ++i) s = a.v[i] > s ? a.v[i] : s; return s; }
template <int N> FN Spec<N> slerp(Real t, const Spec<N>& a, const Spec<N>& b) { Spec<N> r; for (int i = 0; i < N; ++i) r.v[i] = a.v[i] + t * (b.v[i] - a.v[i]); return r; }
template <int N> FN Spec<N> spow(const Spec<N>& a, Real e) { Spec<N> r; for (int i = 0; i < N; ++i) r.v[i] = pow_(max_(a.v[i], (Real)1e-6), e); return r; }

// Counter-based RNG keyed on (pixel, sample, dimension).
// PCG hash: Jarzynski & Olano 2020, "Hash Functions for GPU Rendering" (JCGT 9(3)).
FN uint32_t pcg_hash(uint32_t v) {
  uint32_t state = v * 747796405u + 2891336453u;
  uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
FN uint32_t rng_u32(uint32_t pixel, uint32_t sample, uint32_t dim) {
  return pcg_hash(pcg_hash(pcg_hash(pixel) ^ (sample * 0x9E3779B9u)) + dim * 0x85EBCA6Bu);
}
FN Real u32_to_unit(uint32_t h) { return (Real)(h >> 8) * (Real)(1.0 / 16777216.0); }
struct Rng {
  uint32_t pixel, sample, dim;
  FN Real next() { return u32_to_unit(rng_u32(pixel, sample, dim++)); }
};
FN Rng make_rng(uint32_t pixel, uint32_t sample) { Rng r; r.pixel = pixel; r.sample = sample; r.dim = 0; return r; }
} // namespace kl
