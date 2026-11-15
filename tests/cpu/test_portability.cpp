// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// The CPU half of portability.h and the f32/f64 separation of real.h.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

#include "core/real.h"
#include "portability.h"

AR_NS_BEGIN
// noinline: with inlining each copy could fold its own answer and hide a merge.
__declspec(noinline) inline int real_bytes() { return static_cast<int>(sizeof(Real)); }
AR_NS_END

namespace {

// Every wrapper is checked at a point where its value differs from every other
// wrapper's, against the mathematical value, so a swapped wrapper fails.
template <class T>
bool close_to(T got, double want) {
  const double tol = sizeof(T) == 4 ? 1e-6 : 1e-12;
  return std::fabs(static_cast<double>(got) - want) <= tol * std::fabs(want);
}

template <class T>
void check_wrappers() {
  static_assert(std::is_same<decltype(ar::sqrt(T(1))), T>::value, "wrapper must keep the type");
  const T h = T(0.5);
  CHECK(close_to(ar::sqrt(h), 0.7071067811865476));
  CHECK(close_to(ar::exp(h), 1.6487212707001282));
  CHECK(close_to(ar::expm1(h), 0.6487212707001282));
  CHECK(close_to(ar::log(h), -0.6931471805599453));
  CHECK(close_to(ar::sin(h), 0.479425538604203));
  CHECK(close_to(ar::cos(h), 0.8775825618903728));
  CHECK(close_to(ar::tan(h), 0.5463024898437905));
  CHECK(close_to(ar::acos(h), 1.0471975511965979));
  CHECK(close_to(ar::asin(h), 0.5235987755982989));
  CHECK(close_to(ar::cosh(h), 1.1276259652063807));
  CHECK(close_to(ar::atan2(T(1), T(2)), 0.4636476090008061));
  CHECK(close_to(ar::pow(T(2), h), 1.4142135623730951));
  CHECK(close_to(ar::pow(h, T(3)), 0.125));
  CHECK(ar::fma(T(2), T(3), T(4)) == T(10));
  CHECK(ar::abs(T(-1.5)) == T(1.5));
  CHECK(ar::min(T(1), T(2)) == T(1));
  CHECK(ar::max(T(1), T(2)) == T(2));
  CHECK(ar::floor(T(1.5)) == T(1));
  CHECK(ar::ceil(T(1.5)) == T(2));
  CHECK(ar::floor(T(-1.5)) == T(-2));
  CHECK(ar::ceil(T(-1.5)) == T(-1));
}

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("portability/math wrappers") {
    check_wrappers<float>();
    check_wrappers<double>();
  }

  AR_RT_CASE("portability/atomic_max") {
    uint32_t v = 5;
    CHECK(ar::atomic_max(&v, 3u) == 5u);
    CHECK(v == 5u);
    CHECK(ar::atomic_max(&v, 9u) == 5u);
    CHECK(v == 9u);
    CHECK(ar::atomic_max(&v, 9u) == 9u);
    CHECK(v == 9u);

    uint32_t m = 0;
    std::vector<std::thread> pool;
    for (uint32_t t = 0; t < 4; ++t)
      pool.emplace_back([&m, t] {
        for (uint32_t i = 0; i < 1000; ++i) ar::atomic_max(&m, t * 1000u + i);
      });
    for (auto& th : pool) th.join();
    CHECK(m == 3999u);
  }

  AR_RT_CASE("portability/atomic add and cas") {
    uint32_t u = 7;
    CHECK(ar::atomic_add(&u, 5u) == 7u);
    CHECK(u == 12u);
    float f = 1.5f;
    CHECK(ar::atomic_add(&f, 2.0f) == 1.5f);
    CHECK(f == 3.5f);
    CHECK(ar::atomic_cas(&u, 99u, 1u) == 12u);  // mismatch: unchanged
    CHECK(u == 12u);
    CHECK(ar::atomic_cas(&u, 12u, 1u) == 12u);
    CHECK(u == 1u);

    uint32_t tu = 0;
    float tf = 0.0f;
    std::vector<std::thread> pool;
    for (int t = 0; t < 4; ++t)
      pool.emplace_back([&tu, &tf] {
        for (int i = 0; i < 1000; ++i) {
          ar::atomic_add(&tu, 1u);
          ar::atomic_add(&tf, 1.0f);  // integers below 2^24 add exactly
        }
      });
    for (auto& th : pool) th.join();
    CHECK(tu == 4000u);
    CHECK(tf == 4000.0f);
  }

  AR_RT_CASE("portability/misc") {
    CHECK(!ar::AR_GPU);
    const float x = 1.0f;
    CHECK(ar::load_ro(&x) == 1.0f);
    CHECK(ar::bit_cast<uint32_t>(1.0f) == 0x3f800000u);
    CHECK(ar::bit_cast<float>(0x40000000u) == 2.0f);
    CHECK(ar::lane_id() == 0u);
    CHECK(ar::ballot(true) == 1u);
    CHECK(ar::ballot(false) == 0u);
    const uint64_t a = ar::clock();
    CHECK(ar::clock() >= a);
    AR_ASSERT(true);
  }

  // Both copies call ar::real_bytes(); each must reach its own definition.
  AR_RT_CASE("portability/f32 f64 distinct") {
    CHECK(ar::real_bytes() == static_cast<int>(sizeof(ar::Real)));
#if defined(AR_REAL_DOUBLE)
    CHECK(ar::real_bytes() == 8);
#else
    CHECK(ar::real_bytes() == 4);
#endif
  }
}
