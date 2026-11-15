// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// Vector maths, the shading frame and the Spec/Lambda lane operations.
#include <doctest.h>

#include <cmath>
#include <limits>
#include <cstdint>

#include "core/frame.h"
#include "core/spectrum.h"
#include "core/vec.h"

namespace {

using ar::Real;
using ar::Real3;

// xorshift64*, enough to spread test inputs; the kernel sampler is not under test here.
struct Rng {
  uint64_t s;
  double next() {  // uniform in [-1, 1)
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    const uint64_t r = s * 2685821657736338717ull;
    return static_cast<double>(r >> 11) * (2.0 / 9007199254740992.0) - 1.0;
  }
};

Real3 unit_from_double(double x, double y, double z) {
  const double l = std::sqrt(x * x + y * y + z * z);
  return Real3{Real(x / l), Real(y / l), Real(z / l)};
}

double err(Real3 a, Real3 b) {
  return std::fmax(std::fabs(double(a.x) - double(b.x)),
                   std::fmax(std::fabs(double(a.y) - double(b.y)), std::fabs(double(a.z) - double(b.z))));
}

void check_orthonormal(const ar::Frame& f, Real3 n) {
  const double tol = 1e-6;
  CHECK(err(f.n, n) == 0.0);
  CHECK(std::fabs(double(ar::dot(f.s, f.s)) - 1.0) < tol);
  CHECK(std::fabs(double(ar::dot(f.t, f.t)) - 1.0) < tol);
  CHECK(std::fabs(double(ar::dot(f.n, f.n)) - 1.0) < tol);
  CHECK(std::fabs(double(ar::dot(f.s, f.t))) < tol);
  CHECK(std::fabs(double(ar::dot(f.s, f.n))) < tol);
  CHECK(std::fabs(double(ar::dot(f.t, f.n))) < tol);
  CHECK(err(ar::cross(f.s, f.t), f.n) < tol);  // right-handed
}

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("vec/ops") {
    const Real3 a{1, 2, 3}, b{-4, 5, 6};
    CHECK(ar::dot(a, b) == Real(24));
    CHECK(ar::abs_dot(a, Real3{-1, 0, 0}) == Real(1));
    const Real3 c = ar::cross(a, b);
    CHECK(c.x == Real(-3));
    CHECK(c.y == Real(-18));
    CHECK(c.z == Real(13));
    CHECK(ar::length(Real3{2, 3, 6}) == Real(7));
    CHECK(err(ar::normalize(Real3{0, 3, 4}), Real3{0, Real(0.6), Real(0.8)}) < 1e-6);
    const Real3 s = a + b * Real(2) - Real(0.5) * a;
    CHECK(s.x == Real(-7.5));
    CHECK(s.y == Real(11));
    CHECK(s.z == Real(13.5));
    CHECK((-a).z == Real(-3));
    CHECK((a / Real(2)).y == Real(1));
    const Real3 n{0, 0, 1};
    CHECK(ar::face_forward(n, Real3{0, 0, -2}).z == Real(-1));
    CHECK(ar::face_forward(n, Real3{1, 0, 2}).z == Real(1));
    const ar::Real2 p{1, 2}, q{3, -1};
    CHECK(ar::dot(p, q) == Real(1));
    CHECK((p + q).x == Real(4));
    CHECK((p - q).y == Real(3));
    CHECK((p * Real(2)).y == Real(4));
    CHECK((p / Real(2)).x == Real(0.5));
    CHECK(sizeof(Real3) == 3 * sizeof(Real));
  }

  AR_RT_CASE("vec/spec lanes") {
    const ar::Spec a{{1, 2, 3, 6}}, b{{4, 4, 2, 3}};
    const ar::Spec s = a + b, d = a - b, m = a * b, q = a / b;
    CHECK(s.v[3] == Real(9));
    CHECK(d.v[0] == Real(-3));
    CHECK(m.v[2] == Real(6));
    CHECK(q.v[3] == Real(2));
    CHECK((a * Real(2)).v[1] == Real(4));
    CHECK((Real(2) * a).v[2] == Real(6));
    CHECK((a / Real(2)).v[3] == Real(3));
    CHECK(ar::avg(a) == Real(3));
    CHECK(ar::max_lane(a) == Real(6));
    CHECK(ar::is_finite(a));
    ar::Spec bad = a;
    bad.v[2] = std::numeric_limits<Real>::infinity();
    CHECK(!ar::is_finite(bad));
    bad.v[2] = bad.v[2] - bad.v[2];  // NaN
    CHECK(!ar::is_finite(bad));
    const ar::Lambda l{{400, 500, 600, 700}};
    CHECK(ar::avg(l) == Real(550));
    CHECK(ar::max_lane(l) == Real(700));
    CHECK((l * Real(2)).nm[1] == Real(1000));
    CHECK(sizeof(ar::Spec) == 4 * sizeof(Real));
  }

  AR_RT_CASE("vec/frame orthonormal and round trip") {
    Rng rng{0x9e3779b97f4a7c15ull};
    int count = 0;
    auto run = [&](Real3 n, Real3 v) {
      const ar::Frame f = ar::frame_from_normal(n);
      check_orthonormal(f, n);
      const Real3 w = ar::to_world(f, ar::to_local(f, v));
      CHECK(err(w, v) < 1e-6);
      // Local z is the component along the normal.
      CHECK(std::fabs(double(ar::to_local(f, v).z) - double(ar::dot(v, n))) < 1e-6);
      ++count;
    };
    const Real3 v0{Real(0.3), Real(-0.7), Real(0.2)};
    run(Real3{0, 0, -1}, v0);
    run(Real3{0, 0, 1}, v0);
    run(Real3{1, 0, 0}, v0);
    run(Real3{0, -1, 0}, v0);
    for (int i = 0; i < 10000; ++i) {
      double x, y, z;
      do {
        x = rng.next();
        y = rng.next();
        z = rng.next();
      } while (x * x + y * y + z * z < 1e-3);
      run(unit_from_double(x, y, z), Real3{Real(rng.next()), Real(rng.next()), Real(rng.next())});
    }
    CHECK(count == 10004);
  }

  AR_RT_CASE("vec/frame from tangent") {
    Rng rng{12345u};
    for (int i = 0; i < 1000; ++i) {
      const Real3 n = unit_from_double(rng.next(), rng.next(), rng.next() - 1.5);
      const Real3 t{Real(rng.next()), Real(rng.next()), Real(rng.next())};
      const ar::Frame f = ar::frame_from_normal_tangent(n, t);
      check_orthonormal(f, n);
      // s is the tangent's projection onto the plane: it keeps the tangent's side.
      CHECK(double(ar::dot(f.s, t)) > 0.0);
    }
  }
}
