// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// Rectangle and disk area lamps: 0004 D6 (L = P / (pi A), one-sided), D7, D8; 0004 Verification
// rows D6 (analytic irradiance within 0.5%) and D7.
// Ref: Urena, Fajardo and King 2013 (spherical rectangle sampling); pbrt-v4 Disk::Sample/PDF and
// DiffuseAreaLight::SampleLe. Pinned literals come from a float64 NumPy transcription of those
// algorithms (and of the Van Oosterom-Strackee solid angle for the pdf), not from the code under
// test. Spectral pins use a synthetic illuminant s(lambda) = 0.5 + 0.001 (lambda - 360), lanes
// 450.25, 550.5, 600.75, 700 nm, strength P = 250 and a CONSTANT colour of 1. The lamp used for
// pins: centre (1, 2, 5), axes X = (0.6, 0.8, 0), Y = (-0.48, 0.36, 0.8), so the front (-Z, with
// Z = X x Y) is (-0.64, 0.48, -0.6); rectangle 2 x 1.5, disk radius 1.25.
// Irradiance truths: disk on axis E = pi L r^2 / (r^2 + d^2); rectangle E = pi L F with the
// form factor of a point to a parallel rectangle (Howell C-11, corner rectangles by inclusion and
// exclusion).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "common/stat.h"
#include "light/light.h"

namespace st = astroray::stat;

namespace {

using ar::Lambda;
using ar::LightLeSample;
using ar::LightLiSample;
using ar::LightRecord;
using ar::LightType;
using ar::Real;
using ar::Real2;
using ar::Real3;
using ar::Spec;

constexpr double kPi = 3.14159265358979323846;
constexpr bool kF32 = sizeof(Real) == 4;
constexpr double kPinTol = kF32 ? 5e-5 : 1e-6;
constexpr double kStrength = 250.0;

struct Obs {
  float cmf[3 * ar::kObsSamples];
  float illum[ar::kObsSamples];
  ar::ObserverView view;
  Obs() {
    for (float& c : cmf) c = 0.0f;
    for (int i = 0; i < ar::kObsSamples; ++i) illum[i] = float(0.5 + 0.001 * i);
    view.cmf = cmf;
    view.illum = illum;
  }
};

const double kNm[4] = {450.25, 550.5, 600.75, 700.0};
double illum_at(int lane) { return 0.5 + 0.001 * (kNm[lane] - 360.0); }

Lambda lanes() {
  Lambda l;
  for (int i = 0; i < 4; ++i) l.nm[i] = Real(kNm[i]);
  return l;
}

Real3 r3(double x, double y, double z) { return Real3{Real(x), Real(y), Real(z)}; }
Real2 r2(double x, double y) { return Real2{Real(x), Real(y)}; }

struct D3 {
  double x, y, z;
};
D3 d3(Real3 v) { return D3{double(v.x), double(v.y), double(v.z)}; }
Real3 r3(D3 v) { return r3(v.x, v.y, v.z); }
D3 add3(D3 a, D3 b) { return D3{a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 sub3(D3 a, D3 b) { return D3{a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot3(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double len3(D3 a) { return std::sqrt(dot3(a, a)); }
D3 scale3(D3 a, double s) { return D3{a.x * s, a.y * s, a.z * s}; }
D3 cross3(D3 a, D3 b) { return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
D3 unit3(D3 a) { return scale3(a, 1.0 / len3(a)); }

bool close(double got, double want, double tol) { return std::fabs(got - want) <= tol * std::fmax(std::fabs(want), 1.0); }
bool rel_close(double got, double want, double tol) { return std::fabs(got - want) <= tol * std::fabs(want); }
bool finite3(Real3 v) { return std::isfinite(double(v.x)) && std::isfinite(double(v.y)) && std::isfinite(double(v.z)); }
bool finite_spec(const Spec& s) {
  for (int i = 0; i < 4; ++i)
    if (!std::isfinite(double(s.v[i]))) return false;
  return true;
}
bool zero_spec(const Spec& s) {
  for (int i = 0; i < 4; ++i)
    if (s.v[i] != Real(0)) return false;
  return true;
}

// splitmix64; the kernel sampler is not under test here.
struct Rng {
  uint64_t s;
  double next() {
    s += 0x9E3779B97F4A7C15ull;
    uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
  }
};

// A lamp and its stored (float) frame in double, so tests place contexts relative to what the
// kernel sees.
struct Lamp {
  LightRecord rec;
  D3 c, X, Y, F;  // centre, spanning axes, front (-Z)
  double w, h;    // rectangle; a disk has w = radius, h = 0
};

Lamp make_lamp(LightType type, D3 c, D3 X, D3 Y, double w, double h, double strength = kStrength) {
  Lamp l;
  l.rec = ar::light_make_area(type, r3(c), r3(X), r3(Y), Real(w), Real(h), Real(strength),
                              ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(1)));
  l.c = d3(ar::light_position(l.rec));
  l.X = d3(ar::area_axis(l.rec, 0));
  l.Y = d3(ar::area_axis(l.rec, 1));
  l.F = d3(ar::area_front(l.rec));
  l.w = double(l.rec.size[0]);
  l.h = double(l.rec.size[1]);
  return l;
}

const D3 kC{1, 2, 5}, kX{0.6, 0.8, 0}, kY{-0.48, 0.36, 0.8};
Lamp pin_rect() { return make_lamp(LightType::AREA_RECT, kC, kX, kY, 2.0, 1.5); }
Lamp pin_disk() { return make_lamp(LightType::AREA_DISK, kC, kX, kY, 1.25, 0.0); }

// A point at lamp coordinates (a, b) along X, Y and d in front of the lamp.
D3 at(const Lamp& l, double a, double b, double d) {
  return add3(add3(l.c, scale3(l.X, a)), add3(scale3(l.Y, b), scale3(l.F, d)));
}

ar::LightSampleContext ctx_at(D3 p) { return ar::light_context_point(r3(p), r3(0, 0, 1), r3(0, 0, 1)); }

double area_of(const Lamp& l) { return l.h > 0 ? l.w * l.h : kPi * l.w * l.w; }

// Van Oosterom and Strackee 1983: solid angle of a spherical triangle, and of a rectangle as two.
double tri_sa(D3 a, D3 b, D3 c) {
  a = unit3(a);
  b = unit3(b);
  c = unit3(c);
  return 2.0 * std::atan2(std::fabs(dot3(a, cross3(b, c))), 1.0 + dot3(a, b) + dot3(b, c) + dot3(c, a));
}
// Solid angle at o of the part [xa, xb] x [ya, yb] of the lamp's rectangle (lamp coordinates).
double rect_sa(const Lamp& l, D3 o, double xa, double xb, double ya, double yb) {
  auto pt = [&](double x, double y) { return sub3(add3(l.c, add3(scale3(l.X, x), scale3(l.Y, y))), o); };
  const D3 p00 = pt(xa, ya), p10 = pt(xb, ya), p01 = pt(xa, yb), p11 = pt(xb, yb);
  return tri_sa(p00, p10, p11) + tri_sa(p00, p11, p01);
}
double rect_sa(const Lamp& l, D3 o) { return rect_sa(l, o, -l.w / 2, l.w / 2, -l.h / 2, l.h / 2); }

// Corner form factor of Howell C-11 for signed extents (a, b) at height z, F(-a, b) = -F(a, b).
double corner_ff(double a, double b, double z) {
  const double sa = std::sqrt(a * a + z * z), sb = std::sqrt(b * b + z * z);
  return (a / sa * std::atan(b / sa) + b / sb * std::atan(a / sb)) / (2.0 * kPi);
}
// Form factor from a point at lamp coordinates (px, py), height z, to the whole rectangle.
double rect_ff(const Lamp& l, double px, double py, double z) {
  const double x0 = -l.w / 2 - px, x1 = l.w / 2 - px, y0 = -l.h / 2 - py, y1 = l.h / 2 - py;
  return corner_ff(x1, y1, z) - corner_ff(x0, y1, z) - corner_ff(x1, y0, z) + corner_ff(x0, y0, z);
}

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("lights_area/record layout and validation") {
    // The front is local -Z (Blender's lamp direction): with the identity frame it is -Z, so a
    // context above the lamp (+z) is behind it and one below is in front.
    const Obs obs;
    const Lamp id = make_lamp(LightType::AREA_RECT, D3{0, 0, 0}, D3{1, 0, 0}, D3{0, 1, 0}, 2.0, 2.0);
    CHECK(id.F.z == -1.0);
    CHECK(ar::light_valid(id.rec));
    CHECK(ar::light_power(id.rec) == Real(kStrength));
    const LightLiSample below = ar::light_sample_li(id.rec, ctx_at(D3{0.1, 0.2, -3}), r2(0.5, 0.5), lanes(), obs.view);
    const LightLiSample above = ar::light_sample_li(id.rec, ctx_at(D3{0.1, 0.2, 3}), r2(0.5, 0.5), lanes(), obs.view);
    CHECK(below.pdf > Real(0));
    CHECK(above.pdf == Real(0));
    CHECK(ar::light_valid(pin_disk().rec));

    auto bad_rect = [&](float w, float h) {
      LightRecord b = pin_rect().rec;
      b.size[0] = w;
      b.size[1] = h;
      return b;
    };
    CHECK_FALSE(ar::light_valid(bad_rect(0.0f, 1.0f)));
    CHECK_FALSE(ar::light_valid(bad_rect(1.0f, -1.0f)));
    CHECK(ar::light_valid(bad_rect(1e-18f, 1e-18f)));
    CHECK_FALSE(ar::light_valid(bad_rect(1e-19f, 1e-19f)));  // A = 1e-38: L = P / (pi A) > FLT_MAX
    CHECK(ar::light_problem(bad_rect(1e-19f, 1e-19f)) != nullptr);
    CHECK(ar::light_valid(bad_rect(1e18f, 1e18f)));
    CHECK_FALSE(ar::light_valid(bad_rect(1e19f, 1e19f)));  // A = 1e38: the pdf 1 / A is not a normal float
    LightRecord bad = pin_rect().rec;
    bad.size[0] = bad.size[1] = 0.01f;
    bad.strength = 3e38f;
    CHECK_FALSE(ar::light_valid(bad));  // P / (pi A) = 1e42 overflows
    bad = pin_rect().rec;
    bad.xf[0] *= 2.0f;
    bad.xf[4] *= 2.0f;
    bad.xf[8] *= 2.0f;
    CHECK_FALSE(ar::light_valid(bad));  // scaled axis: scale belongs in the size
    bad = pin_rect().rec;
    bad.xf[1] = bad.xf[0];  // X and Y no longer perpendicular
    CHECK_FALSE(ar::light_valid(bad));
    bad = pin_rect().rec;
    bad.xf[3] = INFINITY;
    CHECK_FALSE(ar::light_valid(bad));
    bad = pin_rect().rec;
    bad.emission.semantic = uint8_t(ar::SpectrumSemantic::REFLECTANCE);
    CHECK_FALSE(ar::light_valid(bad));

    bad = pin_disk().rec;
    bad.size[0] = 1e-18f;  // A = 3e-36
    CHECK(ar::light_valid(bad));
    bad.size[0] = 1e-19f;  // A = 3e-38 < P / (pi FLT_MAX)
    CHECK_FALSE(ar::light_valid(bad));
    bad.size[0] = 0.0f;
    CHECK_FALSE(ar::light_valid(bad));
    bad = pin_disk().rec;
    bad.size[1] = 1.0f;
    CHECK_FALSE(ar::light_valid(bad));  // a disk has no height
  }

  AR_RT_CASE("lights_area/rect pinned samples") {
    const Obs obs;
    const Lamp lamp = pin_rect();
    struct Row {
      double p[3], u0, u1;
      double wi[3], dist, pdf, pl[3];
      double loose;  // tolerance, both builds, where the row needs more than kPinTol (0: kPinTol)
    };
    // NumPy: Urena Algorithms 1 and 2 (pdf 1 / S from Van Oosterom-Strackee) inside pbrt's
    // [3e-4, 6.22] sr, area sampling (pdf d^2 / (cos A)) outside. Rows: a near context (S = 0.268),
    // u = (0, 0.5) on the x0 edge, S = 0.0033, S = 6.1e-4 (just above the range's lower end),
    // S = 1.3e-4 (below it: area), S = 6.256 (above 6.22, a point 4 mm in front of the lamp: area).
    const Row rows[] = {
        {{-0.5, 4.0, 3.0}, 0.3, 0.8, {0.332595662787, -0.626652768365, 0.704759840652}, 3.33151645599, 3.73162787883,
         {0.608047923766, 1.912295990003, 5.347919006652}},
        {{-0.5, 4.0, 3.0}, 0.0, 0.5, {0.254440438229, -0.788370973511, 0.560117194451}, 3.55624344278, 3.73162787883,
         {0.40485214003, 1.196360894978, 4.991913099951}},
        {{-16.4, 18.8, -13.0}, 0.3, 0.8, {0.56124183382, -0.561503688723, 0.608047047128}, 30.1949150457, 304.764622338,
         {0.546649492297, 1.845443821171, 5.35992893182}},
        {{-43.8, 35.6, -37.0}, 0.3, 0.8, {0.633462692969, -0.482239000582, 0.605120287988}, 70.0025889357, 1633.593741996,
         {0.544028501995, 1.842021473498, 5.359986776671}, 3e-3},  // S = 6.1e-4: float S is good to 1e-6 absolute
        {{-93.2, 76.4, -85.0}, 0.3, 0.8, {0.624796706422, -0.496923460034, 0.602242601044}, 150.039203210, 7505.88201858,
         {0.544, 1.842, 5.36}},
        {{1.03344, 2.09992, 5.0376}, 0.3, 0.8, {-0.764361945954, -0.402795507315, 0.503494384144}, 0.64032491752, 21.8786217819,
         {0.544, 1.842, 5.36}, 2e-5}};  // cos at the sample is 0.006: the float frame's 6e-8 is amplified
    for (const Row& row : rows) {
      const LightLiSample s = ar::light_sample_li(lamp.rec, ctx_at(D3{row.p[0], row.p[1], row.p[2]}), r2(row.u0, row.u1), lanes(), obs.view);
      const double tol = row.loose > 0 ? row.loose : kPinTol;
      INFO("o=(" << row.p[0] << "," << row.p[1] << "," << row.p[2] << ") u=(" << row.u0 << "," << row.u1 << ")");
      CHECK(close(double(s.wi.x), row.wi[0], tol));
      CHECK(close(double(s.wi.y), row.wi[1], tol));
      CHECK(close(double(s.wi.z), row.wi[2], tol));
      CHECK(rel_close(double(s.dist), row.dist, tol));
      CHECK(rel_close(double(s.pdf), row.pdf, tol));
      CHECK(close(double(s.p_light.x), row.pl[0], tol));
      CHECK(close(double(s.p_light.y), row.pl[1], tol));
      CHECK(close(double(s.p_light.z), row.pl[2], tol));
      CHECK_FALSE(s.delta);
      CHECK_FALSE(s.infinite);
      // L = P s / (pi A), A = 3 (NumPy).
      const double radiance[4] = {15.6568675267, 18.3160813675, 19.6490040159, 22.2816920329};
      for (int i = 0; i < 4; ++i) CHECK(rel_close(double(s.Le.v[i]), radiance[i], tol));
    }
  }

  AR_RT_CASE("lights_area/disk pinned samples") {
    const Obs obs;
    const Lamp lamp = pin_disk();
    struct Row {
      double u0, u1, wi[3], dist, pdf, pl[3];
    };
    // NumPy: concentric disk point, pdf d^2 / (cos A) with A = pi r^2. u = (0, 0.5) is the
    // rim point (0.25, 1, 5).
    const Row rows[] = {{0.3, 0.8, {0.283482926479, -0.608083519954, 0.741533453834}, 3.39784433088, 2.56145034825,
                         {0.463230854638, 1.933826859022, 5.519615242271}},
                        {0.0, 0.5, {0.203653269991, -0.814613079963, 0.543075386642}, 3.68272996566, 3.26125782438,
                         {0.25, 1.0, 5.0}}};
    for (const Row& row : rows) {
      const LightLiSample s = ar::light_sample_li(lamp.rec, ctx_at(D3{-0.5, 4.0, 3.0}), r2(row.u0, row.u1), lanes(), obs.view);
      INFO("u=(" << row.u0 << "," << row.u1 << ")");
      CHECK(close(double(s.wi.x), row.wi[0], kPinTol));
      CHECK(close(double(s.wi.y), row.wi[1], kPinTol));
      CHECK(close(double(s.wi.z), row.wi[2], kPinTol));
      CHECK(rel_close(double(s.dist), row.dist, kPinTol));
      CHECK(rel_close(double(s.pdf), row.pdf, kPinTol));
      CHECK(close(double(s.p_light.x), row.pl[0], kPinTol));
      CHECK(close(double(s.p_light.y), row.pl[1], kPinTol));
      CHECK(close(double(s.p_light.z), row.pl[2], kPinTol));
      CHECK_FALSE(s.delta);
    }
    // L = P s / (pi A) = P s / (pi^2 r^2) (NumPy).
    const double radiance[4] = {9.56877258318, 11.1939643688, 12.0085866853, 13.6175670815};
    const LightLiSample s = ar::light_sample_li(lamp.rec, ctx_at(D3{-0.5, 4.0, 3.0}), r2(0.3, 0.8), lanes(), obs.view);
    for (int i = 0; i < 4; ++i) CHECK(rel_close(double(s.Le.v[i]), radiance[i], kPinTol));
  }

  AR_RT_CASE("lights_area/rect pdf consistency") {
    // pdf_li(sample.wi) == sample.pdf within 1e-4, and the pdf is the independent one: 1 / S
    // from the Van Oosterom-Strackee solid angle where the lamp is spherical-rectangle sampled,
    // d^2 / (cos A) where it is area sampled. Samples lie on the rectangle.
    const Obs obs;
    const Lamp lamp = pin_rect();
    // 0.003 above the middle of the lamp has S > 6.22; 70 gives S just above 3e-4, 200 below it.
    const double heights[] = {0.003, 0.5, 1.0, 3.0, 10.0, 30.0, 70.0, 100.0, 200.0};
    const double offs[][2] = {{0, 0}, {2.5, -1.0}, {-8.0, 6.0}};
    int sph = 0, area = 0, above = 0;
    for (double d : heights)
      for (const auto& off : offs) {
        const D3 p = at(lamp, off[0], off[1], d);
        const ar::LightSampleContext ctx = ctx_at(p);
        const double S = rect_sa(lamp, p);
        for (int i = 0; i < 8; ++i)
          for (int j = 0; j < 8; ++j) {
            const LightLiSample s = ar::light_sample_li(lamp.rec, ctx, r2((i + 0.5) / 8, (j + 0.5) / 8), lanes(), obs.view);
            INFO("d=" << d << " off=(" << off[0] << "," << off[1] << ") i=" << i << " j=" << j);
            REQUIRE(s.pdf > Real(0));
            const ar::LightHit hit{s.p_light, r3(lamp.F)};
            CHECK(rel_close(double(ar::light_pdf_li(lamp.rec, ctx, s.wi, hit)), double(s.pdf), 1e-4));
            // Where the sample is: in the rectangle's plane and extent.
            const D3 rel = sub3(d3(s.p_light), lamp.c);
            // The stored axes are float even in the f64 build, so they are not exactly perpendicular.
            const double tol = (kF32 ? 2e-6 * (d + 10.0) : 5e-7);
            CHECK(std::fabs(dot3(rel, lamp.F)) <= tol);
            CHECK(std::fabs(dot3(rel, lamp.X)) <= lamp.w / 2 + tol);
            CHECK(std::fabs(dot3(rel, lamp.Y)) <= lamp.h / 2 + tol);
            const D3 to = sub3(d3(s.p_light), p);
            CHECK(std::fabs(len3(to) - double(s.dist)) <= 1e-4 * double(s.dist));
            CHECK(dot3(unit3(to), d3(s.wi)) >= 1.0 - 1e-6);
            // The independent pdf.
            if (S >= 6e-4 && S <= 6.2) {
              // float S is a sum of angles near pi/2, so it carries an absolute error of about 1e-6
              CHECK(rel_close(double(s.pdf), 1.0 / S, kF32 ? std::fmax(1e-3, 5e-6 / S) : 1e-3));
              ++sph;
            } else if (S < 2e-4 || S > 6.23) {
              const double dist = double(s.dist);
              const double cosl = dot3(lamp.F, scale3(unit3(to), -1.0));
              CHECK(rel_close(double(s.pdf), dist * dist / (cosl * lamp.w * lamp.h), 2e-3));
              ++area;
              if (S > 6.23) ++above;
            }
          }
      }
    CHECK(sph > 100);  // both branches were exercised
    CHECK(area > 100);
    CHECK(above > 0);
  }

  AR_RT_CASE("lights_area/disk pdf consistency") {
    const Obs obs;
    const Lamp lamp = pin_disk();
    const double heights[] = {0.3, 1.0, 3.0, 10.0, 100.0};
    const double offs[][2] = {{0, 0}, {2.0, -1.0}, {-8.0, 6.0}};
    for (double d : heights)
      for (const auto& off : offs) {
        const D3 p = at(lamp, off[0], off[1], d);
        const ar::LightSampleContext ctx = ctx_at(p);
        for (int i = 0; i < 8; ++i)
          for (int j = 0; j < 8; ++j) {
            const LightLiSample s = ar::light_sample_li(lamp.rec, ctx, r2((i + 0.5) / 8, (j + 0.5) / 8), lanes(), obs.view);
            INFO("d=" << d << " off=(" << off[0] << "," << off[1] << ") i=" << i << " j=" << j);
            REQUIRE(s.pdf > Real(0));
            const ar::LightHit hit{s.p_light, r3(lamp.F)};
            CHECK(rel_close(double(ar::light_pdf_li(lamp.rec, ctx, s.wi, hit)), double(s.pdf), 1e-4));
            const D3 rel = sub3(d3(s.p_light), lamp.c);
            // The stored axes are float even in the f64 build, so they are not exactly perpendicular.
            const double tol = (kF32 ? 2e-6 * (d + 10.0) : 5e-7);
            CHECK(std::fabs(dot3(rel, lamp.F)) <= tol);
            CHECK(len3(rel) <= lamp.w + tol);
            // Independent: (1 / A) d^2 / cos.
            const D3 to = sub3(d3(s.p_light), p);
            const double cosl = dot3(lamp.F, scale3(unit3(to), -1.0));
            CHECK(rel_close(double(s.pdf), dot3(to, to) / (cosl * kPi * lamp.w * lamp.w), 2e-4));
            CHECK(dot3(unit3(to), d3(s.wi)) >= 1.0 - 1e-6);
          }
      }
  }

  // Irradiance on a surface facing the lamp, parallel to it: E = sum Le cos / pdf over a
  // midpoint grid of the two uniforms (deterministic), against L G with G the geometric closed
  // form, within 0.5% (0004 V D6).
  static void irradiance_check(const Lamp& lamp, double px, double py, double d, double geometric, const char* what) {
    const Obs obs;
    const D3 p = at(lamp, px, py, d);
    const ar::LightSampleContext ctx = ctx_at(p);
    const D3 n = scale3(lamp.F, -1.0);  // the receiver faces the lamp
    constexpr int kG = 256;
    double acc[4] = {0, 0, 0, 0};
    for (int i = 0; i < kG; ++i)
      for (int j = 0; j < kG; ++j) {
        const LightLiSample s = ar::light_sample_li(lamp.rec, ctx, r2((i + 0.5) / kG, (j + 0.5) / kG), lanes(), obs.view);
        if (!(s.pdf > Real(0))) continue;
        const double cosn = std::fmax(0.0, dot3(n, d3(s.wi)));
        for (int l = 0; l < 4; ++l) acc[l] += double(s.Le.v[l]) * cosn / double(s.pdf);
      }
    for (int l = 0; l < 4; ++l) {
      const double got = acc[l] / (double(kG) * kG);
      const double want = kStrength * illum_at(l) / (kPi * area_of(lamp)) * geometric;
      INFO(what << " d=" << d << " lane " << l << " got " << got << " want " << want);
      CHECK(rel_close(got, want, 5e-3));
    }
  }

  AR_RT_CASE("lights_area/disk on-axis irradiance") {
    // E = pi L r^2 / (r^2 + d^2) on the axis.
    const Lamp lamp = pin_disk();
    const double r = lamp.w;
    for (double d : {0.2, 0.5, 1.25, 4.0, 12.0}) irradiance_check(lamp, 0, 0, d, kPi * r * r / (r * r + d * d), "disk");
  }

  AR_RT_CASE("lights_area/square on-axis irradiance") {
    // Square 2 x 2 above its centre, a 2 x 1.5 rectangle above a corner and above an arbitrary
    // point, and a far point (area sampled branch). G = pi F.
    const Lamp sq = make_lamp(LightType::AREA_RECT, kC, kX, kY, 2.0, 2.0);
    for (double d : {0.3, 1.0, 5.0}) irradiance_check(sq, 0, 0, d, kPi * rect_ff(sq, 0, 0, d), "square centre");
    const Lamp rc = pin_rect();
    irradiance_check(rc, 1.0, 0.75, 1.5, kPi * rect_ff(rc, 1.0, 0.75, 1.5), "corner");
    irradiance_check(rc, -2.3, 1.1, 2.0, kPi * rect_ff(rc, -2.3, 1.1, 2.0), "off to the side");
    irradiance_check(rc, 3.0, 0.0, 30.0, kPi * rect_ff(rc, 3.0, 0.0, 30.0), "far");
    // Area-sampled range S = 7.5e-5 < 3e-4. S > 6.22 needs the point within millimetres of the lamp's
    // plane, where uniform area sampling cannot resolve E on a grid; that branch is pinned and
    // pdf-checked above instead.
    irradiance_check(rc, 0.0, 0.0, 200.0, kPi * rect_ff(rc, 0.0, 0.0, 200.0), "far, area sampled");
    // A square's closed form against the independent fact that four quarters add up.
    CHECK(rel_close(rect_ff(sq, 0, 0, 1.0), 4.0 * corner_ff(1.0, 1.0, 1.0), 1e-12));
  }

  AR_RT_CASE("lights_area/behind and edge-on") {
    // One-sided (D8): a context behind the lamp, or in its plane, gets no sample, no pdf and no
    // radiance; light paths leave the front only.
    const Obs obs;
    for (const Lamp& lamp : {pin_rect(), pin_disk()}) {
      for (double d : {-3.0, -0.001}) {
        for (double off : {0.0, 0.7}) {
          const D3 p = at(lamp, off, 0.1, d);
          const ar::LightSampleContext ctx = ctx_at(p);
          const LightLiSample s = ar::light_sample_li(lamp.rec, ctx, r2(0.4, 0.6), lanes(), obs.view);
          INFO("d=" << d << " off=" << off);
          CHECK(s.pdf == Real(0));
          CHECK(zero_spec(s.Le));
          const ar::LightHit hit{r3(lamp.c), r3(lamp.F)};
          CHECK(ar::light_pdf_li(lamp.rec, ctx, r3(unit3(sub3(lamp.c, p))), hit) == Real(0));
        }
      }
      // Exactly in the plane (an identity frame, so no rounding moves the point off it).
      const Lamp flat = make_lamp(static_cast<LightType>(lamp.rec.type), D3{0, 0, 0}, D3{1, 0, 0}, D3{0, 1, 0}, lamp.w, lamp.h);
      for (double px : {0.3, 4.0}) {
        const ar::LightSampleContext ctx = ctx_at(D3{px, 0.2, 0});
        const LightLiSample s = ar::light_sample_li(flat.rec, ctx, r2(0.4, 0.6), lanes(), obs.view);
        CHECK(s.pdf == Real(0));
        CHECK(zero_spec(s.Le));
        CHECK(ar::light_pdf_li(flat.rec, ctx, r3(-1, 0, 0), ar::LightHit{r3(0.1, 0.1, 0), r3(0, 0, -1)}) == Real(0));
      }
      const ar::SceneSphere world{r3(0, 0, 0), Real(100)};
      Rng rng{st::mix_bits(7)};
      for (int k = 0; k < 256; ++k) {
        const LightLeSample e = ar::light_sample_le(lamp.rec, r2(rng.next(), rng.next()), r2(rng.next(), rng.next()), lanes(), obs.view, world);
        if (e.pdf_dir == Real(0)) {
          CHECK(zero_spec(e.Le));
          continue;
        }
        CHECK(dot3(d3(e.ray.d), lamp.F) > 0.0);
        // The origin is on the lamp's plane.
        CHECK(std::fabs(dot3(sub3(d3(e.ray.o), lamp.c), lamp.F)) <= 1e-5);
      }
    }
  }

  AR_RT_CASE("lights_area/disk power") {
    // light_power is the record's P times the radiance scale, the unweighted scalar of D10: for
    // a record built with strength 731.25 it is exactly 731.25, for every lamp shape and for
    // radii and sizes that change A (power does not depend on the size).
    for (double radius : {0.05, 1.25, 37.0}) {
      const Lamp d = make_lamp(LightType::AREA_DISK, kC, kX, kY, radius, 0.0, 731.25);
      CHECK(double(ar::light_power(d.rec)) == 731.25);
    }
    const Lamp r = make_lamp(LightType::AREA_RECT, kC, kX, kY, 2.0, 1.5, 731.25);
    CHECK(double(ar::light_power(r.rec)) == 731.25);
    // And it is what the radiance radiates: L * pi * A recovers it from the disk's own radiance.
    const Obs obs;
    const Lamp d = make_lamp(LightType::AREA_DISK, kC, kX, kY, 1.25, 0.0, 731.25);
    const Spec rad = ar::disk_radiance(d.rec, lanes(), obs.view);
    for (int l = 0; l < 4; ++l) CHECK(rel_close(double(rad.v[l]) * kPi * area_of(d), 731.25 * illum_at(l), 1e-4));
  }

  AR_RT_CASE("lights_area/power from radiance") {
    // L reproduces P: flux = L * (cosine integral over the hemisphere, pi) * A by midpoint
    // quadrature, and by the Le sampling route Le cos / (pdf_pos pdf_dir). 1e-4 (0004 V D6).
    const Obs obs;
    const ar::SceneSphere world{r3(0, 0, 0), Real(100)};
    for (const Lamp& lamp : {pin_rect(), make_lamp(LightType::AREA_RECT, kC, kX, kY, 0.05, 0.2),
                             make_lamp(LightType::AREA_RECT, kC, kX, kY, 30.0, 30.0), pin_disk(),
                             make_lamp(LightType::AREA_DISK, kC, kX, kY, 0.05, 0.0), make_lamp(LightType::AREA_DISK, kC, kX, kY, 37.0, 0.0)}) {
      const bool rect = lamp.rec.type == uint8_t(LightType::AREA_RECT);
      const Spec rad = rect ? ar::rect_radiance(lamp.rec, lanes(), obs.view) : ar::disk_radiance(lamp.rec, lanes(), obs.view);
      constexpr int kNz = 512;
      double hemi = 0.0;
      for (int i = 0; i < kNz; ++i) hemi += (i + 0.5) / kNz * (1.0 / kNz) * 2.0 * kPi;
      for (int l = 0; l < 4; ++l) {
        INFO("w=" << lamp.w << " h=" << lamp.h << " lane " << l);
        CHECK(rel_close(double(rad.v[l]) * hemi * area_of(lamp), kStrength * illum_at(l), 1e-4));
      }
      Rng rng{st::mix_bits(uint64_t(lamp.w * 1000) + (rect ? 3 : 5))};
      // The first draws are fixed: u_dir on the rim of the concentric disk is the horizon.
      const Real2 fixed_ud[4] = {r2(0, 0), r2(0, 0.5), r2(0.5, 0.5), r2(0.25, 0.75)};
      for (int k = 0; k < 200; ++k) {
        const Real2 up = r2(rng.next(), rng.next());
        const Real2 ud = k < 4 ? fixed_ud[k] : r2(rng.next(), rng.next());
        const LightLeSample s = ar::light_sample_le(lamp.rec, up, ud, lanes(), obs.view, world);
        if (!(s.pdf_dir > Real(0))) {
          CHECK(zero_spec(s.Le));
          continue;
        }
        if (double(s.pdf_dir) * kPi < 1e-2) continue;  // float direction rounding vs a tiny cosine
        const double cosd = dot3(lamp.F, d3(s.ray.d));
        for (int l = 0; l < 4; ++l)
          CHECK(rel_close(double(s.Le.v[l]) * cosd / (double(s.pdf_pos) * double(s.pdf_dir)), kStrength * illum_at(l), 2e-4));
      }
    }
  }

  AR_RT_CASE("lights_area/sample_le and bounds") {
    const Obs obs;
    const ar::SceneSphere world{r3(0, 0, 0), Real(100)};
    // NumPy: area point from u_pos = (0.3, 0.8), cosine hemisphere about the front normal from
    // u_dir = (0.6, 0.2) through the Duff frame; the same direction for both lamps.
    const double dir[3] = {-0.285188523078, 0.909915458378, -0.301199875348};
    const LightLeSample re = ar::light_sample_le(pin_rect().rec, r2(0.3, 0.8), r2(0.6, 0.2), lanes(), obs.view, world);
    CHECK(close(double(re.ray.o.x), 0.544, kPinTol));
    CHECK(close(double(re.ray.o.y), 1.842, kPinTol));
    CHECK(close(double(re.ray.o.z), 5.36, kPinTol));
    CHECK(close(double(re.ray.d.x), dir[0], kPinTol));
    CHECK(close(double(re.ray.d.y), dir[1], kPinTol));
    CHECK(close(double(re.ray.d.z), dir[2], kPinTol));
    CHECK(rel_close(double(re.pdf_pos), 1.0 / 3.0, kPinTol));
    CHECK(rel_close(double(re.pdf_dir), 0.254647908947, kPinTol));
    CHECK(rel_close(double(re.Le.v[1]), 18.3160813675, kPinTol));

    const LightLeSample de = ar::light_sample_le(pin_disk().rec, r2(0.3, 0.8), r2(0.6, 0.2), lanes(), obs.view, world);
    CHECK(close(double(de.ray.o.x), 0.463230854638, kPinTol));
    CHECK(close(double(de.ray.o.y), 1.933826859022, kPinTol));
    CHECK(close(double(de.ray.o.z), 5.519615242271, kPinTol));
    CHECK(close(double(de.ray.d.x), dir[0], kPinTol));
    CHECK(close(double(de.ray.d.z), dir[2], kPinTol));
    CHECK(rel_close(double(de.pdf_pos), 0.203718327158, kPinTol));
    CHECK(rel_close(double(de.pdf_dir), 0.254647908947, kPinTol));
    CHECK(rel_close(double(de.Le.v[1]), 11.1939643688, kPinTol));

    // NumPy: min and max of the four corners; the exact extent r sqrt(1 - n_i^2) of the disk.
    const ar::LightBounds rb = ar::light_bounds(pin_rect().rec);
    CHECK(close(double(rb.bbox.lo.x), 0.04, kPinTol));
    CHECK(close(double(rb.bbox.lo.y), 0.93, kPinTol));
    CHECK(close(double(rb.bbox.lo.z), 4.4, kPinTol));
    CHECK(close(double(rb.bbox.hi.x), 1.96, kPinTol));
    CHECK(close(double(rb.bbox.hi.y), 3.07, kPinTol));
    CHECK(close(double(rb.bbox.hi.z), 5.6, kPinTol));
    CHECK(close(double(rb.theta_o), 0.0, kPinTol));
    CHECK(rel_close(double(rb.theta_e), kPi / 2, kPinTol));
    CHECK_FALSE(rb.two_sided);
    const ar::LightBounds db = ar::light_bounds(pin_disk().rec);
    CHECK(close(double(db.bbox.lo.x), 0.039531364385, kPinTol));
    CHECK(close(double(db.bbox.lo.y), 0.903414390027, kPinTol));
    CHECK(close(double(db.bbox.lo.z), 4.0, kPinTol));
    CHECK(close(double(db.bbox.hi.x), 1.960468635615, kPinTol));
    CHECK(close(double(db.bbox.hi.y), 3.096585609973, kPinTol));
    CHECK(close(double(db.bbox.hi.z), 6.0, kPinTol));
    CHECK(close(double(db.theta_o), 0.0, kPinTol));
    CHECK(rel_close(double(db.theta_e), kPi / 2, kPinTol));
    CHECK_FALSE(db.two_sided);
  }

  AR_RT_CASE("lights_area/boundary uniforms") {
    // u = 0 and the largest Real below 1 in every combination, for contexts in front, behind,
    // in the plane and almost in it: finite outputs, and pdf 0 means no sample with Le 0.
    const Obs obs;
    const Real lo = Real(0), hi = std::nextafter(Real(1), Real(0));
    const Real vals[] = {lo, hi};
    const ar::SceneSphere world{r3(0, 0, 0), Real(100)};
    const double sizes[] = {1.0, 1e-3, 1e-9};
    const double heights[] = {1e-6, 1e-3, 1.0, 1e3, 1e6, 0.0, -1.0};
    const double offs[][2] = {{0, 0}, {0.5, 0.5}, {3.0, -2.0}, {1e3, 0.0}};
    for (LightType type : {LightType::AREA_RECT, LightType::AREA_DISK})
      for (double size : sizes) {
        const Lamp lamp = make_lamp(type, kC, kX, kY, size, type == LightType::AREA_RECT ? size : 0.0);
        for (double d : heights)
          for (const auto& off : offs)
            for (Real a : vals)
              for (Real b : vals) {
                const D3 p = at(lamp, off[0], off[1], d);
                const ar::LightSampleContext ctx = ctx_at(p);
                const LightLiSample s = ar::light_sample_li(lamp.rec, ctx, Real2{a, b}, lanes(), obs.view);
                INFO("type=" << int(type) << " size=" << size << " d=" << d << " off=(" << off[0] << "," << off[1] << ") u=(" << double(a) << "," << double(b) << ")");
                CHECK(finite3(s.wi));
                CHECK(finite3(s.p_light));
                CHECK(std::isfinite(double(s.dist)));
                CHECK(std::isfinite(double(s.pdf)));
                CHECK(finite_spec(s.Le));
                if (s.pdf == Real(0)) {
                  CHECK(zero_spec(s.Le));
                } else {
                  CHECK(s.pdf > Real(0));
                  CHECK(s.dist > Real(0));
                }
                const ar::LightHit hit{s.p_light, r3(lamp.F)};
                const Real pdf = ar::light_pdf_li(lamp.rec, ctx, s.wi, hit);
                CHECK(std::isfinite(double(pdf)));
                CHECK(pdf >= Real(0));
                const LightLeSample e = ar::light_sample_le(lamp.rec, Real2{a, b}, Real2{b, a}, lanes(), obs.view, world);
                CHECK(finite3(e.ray.o));
                CHECK(finite3(e.ray.d));
                CHECK(finite_spec(e.Le));
                CHECK(std::isfinite(double(e.pdf_pos)));
                CHECK(std::isfinite(double(e.pdf_dir)));
                if (e.pdf_pos == Real(0) || e.pdf_dir == Real(0)) {
                  CHECK(zero_spec(e.Le));
                  CHECK(e.pdf_pos == Real(0));
                  CHECK(e.pdf_dir == Real(0));
                }
              }
        // u_dir on the rim of the concentric disk is the horizon: pdf_dir 0 comes with Le 0 in
        // both builds ((0, 0.5) gives a rim point of exactly unit length).
        const Real2 rim[] = {r2(0, 0), r2(0, 0.5), r2(0.5, 0)};
        int horizon = 0;
        for (const Real2& ud : rim)
          for (Real a : vals)
            for (Real b : vals) {
              const LightLeSample e = ar::light_sample_le(lamp.rec, Real2{a, b}, ud, lanes(), obs.view, world);
              if (e.pdf_dir == Real(0)) {
                ++horizon;
                CHECK(zero_spec(e.Le));
                CHECK(e.pdf_pos == Real(0));
              } else {
                CHECK(e.pdf_pos > Real(0));
                CHECK(finite_spec(e.Le));
              }
            }
        CHECK(horizon >= 4);
      }
  }
}

TEST_SUITE("gate") {
  // Chi-squared of the sampled points against the pdf: the rectangle is cut into a grid of cells
  // (lamp coordinates); a cell's expected count is n * pdf_li * (its solid angle, by the
  // independent Van Oosterom-Strackee formula), so the pdf under test sets the expectation, and
  // pdf * S must be 1.
  static void chi2_rect(double ox, double oy, double d, double w, double h, const char* name) {
    const st::Run run = st::begin(name, st::gate_family());
    const Obs obs;
    const Lamp lamp = make_lamp(LightType::AREA_RECT, kC, kX, kY, w, h);
    const D3 p = at(lamp, ox, oy, d);
    const ar::LightSampleContext ctx = ctx_at(p);
    const double pdf = double(ar::light_pdf_li(lamp.rec, ctx, r3(unit3(sub3(lamp.c, p))), ar::LightHit{r3(lamp.c), r3(lamp.F)}));
    const uint64_t seed = st::mix_bits(run.seed ^ uint64_t(d * 100 + w));
    auto gen = [&](int ires) {
      const int nx = ires, ny = ires, cells = nx * ny, n = 2000 * cells;
      st::Histogram hist;
      hist.observed.assign(cells, 0.0);
      hist.expected.assign(cells, 0.0);
      for (int i = 0; i < nx; ++i)
        for (int j = 0; j < ny; ++j)
          hist.expected[i * ny + j] = n * pdf * rect_sa(lamp, p, -w / 2 + w * i / nx, -w / 2 + w * (i + 1) / nx, -h / 2 + h * j / ny, -h / 2 + h * (j + 1) / ny);
      Rng rng{seed};
      for (int k = 0; k < n; ++k) {
        const LightLiSample s = ar::light_sample_li(lamp.rec, ctx, r2(rng.next(), rng.next()), lanes(), obs.view);
        const D3 rel = sub3(d3(s.p_light), lamp.c);
        const int i = std::min(nx - 1, std::max(0, int((dot3(rel, lamp.X) + w / 2) / w * nx)));
        const int j = std::min(ny - 1, std::max(0, int((dot3(rel, lamp.Y) + h / 2) / h * ny)));
        hist.observed[i * ny + j] += 1.0;
      }
      return hist;
    };
    const st::Chi2Result res = st::chi2_with_retry(run, gen, 8);
    std::printf("lights_area chi2 %s: chi2 %.1f dof %d p %.4f%s\n", name, res.chi2, res.dof, res.p, res.retried ? " (retried at 16)" : "");
    CHECK_MESSAGE(res.pass, res.message);
    // Float S is a sum of angles near pi/2, good to about 1e-6 absolute: that is 1e-3 relative at S = 6e-4.
    const double S = rect_sa(lamp, p);
    CHECK(rel_close(pdf * S, 1.0, kF32 ? std::fmax(1e-4, 5e-6 / S) : 1e-4));
  }

  AR_RT_CASE("lights_area/rect chi2 above") { chi2_rect(0.0, 0.0, 1.5, 2.0, 1.5, "lights_area/rect chi2 above" AR_RT_SUFFIX); }
  AR_RT_CASE("lights_area/rect chi2 oblique") { chi2_rect(3.0, -2.0, 2.0, 2.0, 1.5, "lights_area/rect chi2 oblique" AR_RT_SUFFIX); }
  AR_RT_CASE("lights_area/rect chi2 near the lower end") { chi2_rect(0.0, 0.0, 70.0, 2.0, 1.5, "lights_area/rect chi2 near the lower end" AR_RT_SUFFIX); }
  AR_RT_CASE("lights_area/rect chi2 thin and far") { chi2_rect(4.0, 3.0, 5.0, 3.0, 0.4, "lights_area/rect chi2 thin and far" AR_RT_SUFFIX); }
}
