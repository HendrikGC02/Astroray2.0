// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// Point (radius 0) and sphere (radius > 0) lamps: 0004 D6 (energy conventions), D7 (interface),
// D1 (delta lights), 0003 d11 (unit anchor, factor 1); 0004 Verification rows D6 and D7/D11.
// Ref: pbrt-v4 PointLight::SampleLi/SampleLe and Sphere::Sample(ctx, u) / PDF (cone branch).
// Pinned literals come from a float64 NumPy transcription of those functions (Duff frame,
// SampleUniformSphere, concentric disk), not from the code under test. Spectral pins use a
// synthetic illuminant s(lambda) = 0.5 + 0.001 (lambda - 360), lanes 450.25, 550.5, 600.75, 700
// nm, strength P = 250 and a CONSTANT colour of 1. Irradiance truth is the closed form
// E = P s cos(theta_n) / (4 pi d^2): a Lambertian sphere above the horizon irradiates exactly
// like a point of intensity P s / (4 pi) at its centre.
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
using ar::Real;
using ar::Real2;
using ar::Real3;
using ar::Spec;

constexpr double kPi = 3.14159265358979323846;
constexpr bool kF32 = sizeof(Real) == 4;
// The record and the observer tables store float on both builds, so even the f64 build sees
// inputs rounded to 6e-8 relative.
constexpr double kPinTol = kF32 ? 5e-5 : 1e-6;
constexpr double kStrength = 250.0;

// Synthetic observer tables: only the illuminant matters to a light.
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

LightRecord make_point(Real3 pos, double strength = kStrength) {
  return ar::light_make(ar::LightType::POINT, pos, Real(0), Real(strength),
                        ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(1)));
}
LightRecord make_sphere(Real3 pos, double radius, double strength = kStrength) {
  return ar::light_make(ar::LightType::SPHERE, pos, Real(radius), Real(strength),
                        ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(1)));
}
ar::LightSampleContext ctx_at(Real3 p) { return ar::light_context_point(p, r3(0, 0, 1), r3(0, 0, 1)); }

bool close(double got, double want, double tol) { return std::fabs(got - want) <= tol * std::fmax(std::fabs(want), 1.0); }
bool rel_close(double got, double want, double tol) { return std::fabs(got - want) <= tol * std::fabs(want); }

bool finite3(Real3 v) { return std::isfinite(double(v.x)) && std::isfinite(double(v.y)) && std::isfinite(double(v.z)); }
bool finite_spec(const Spec& s) {
  for (int i = 0; i < 4; ++i)
    if (!std::isfinite(double(s.v[i]))) return false;
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

struct D3 {
  double x, y, z;
};
D3 d3(Real3 v) { return D3{double(v.x), double(v.y), double(v.z)}; }
D3 sub3(D3 a, D3 b) { return D3{a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot3(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double len3(D3 a) { return std::sqrt(dot3(a, a)); }
D3 scale3(D3 a, double s) { return D3{a.x * s, a.y * s, a.z * s}; }
D3 cross3(D3 a, D3 b) { return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
D3 unit3(D3 a) { return scale3(a, 1.0 / len3(a)); }

// Exact uniform cone pdf in a form that does not cancel: 1 / (2 pi (1 - sqrt(1 - s2))).
double cone_pdf(double radius, double dist) {
  const double s2 = (radius / dist) * (radius / dist);
  return (1.0 + std::sqrt(1.0 - s2)) / (2.0 * kPi * s2);
}

}  // namespace

TEST_SUITE("fast") {
  AR_RT_CASE("lights_point/record layout and validation") {
    CHECK(sizeof(LightRecord) == 92);
    const LightRecord pt = make_point(r3(1, 2, 3));
    const LightRecord sp = make_sphere(r3(1, 2, 3), 0.5);
    CHECK(ar::light_valid(pt));
    CHECK(ar::light_valid(sp));
    CHECK(ar::light_position(sp).z == Real(3));
    CHECK(ar::light_power(pt) == Real(kStrength));
    CHECK(ar::light_power(sp) == Real(kStrength));

    LightRecord bad = sp;
    bad.size[0] = -0.5f;
    CHECK_FALSE(ar::light_valid(bad));  // negative radius
    bad.size[0] = 0.0f;
    CHECK_FALSE(ar::light_valid(bad));  // a sphere has a positive radius
    bad = pt;
    bad.size[0] = 0.25f;
    CHECK_FALSE(ar::light_valid(bad));  // a point lamp has radius 0
    bad = pt;
    bad.strength = -1.0f;
    CHECK_FALSE(ar::light_valid(bad));
    bad.strength = std::nanf("");
    CHECK_FALSE(ar::light_valid(bad));
    bad = pt;
    bad.xf[7] = INFINITY;
    CHECK_FALSE(ar::light_valid(bad));
    bad = pt;
    bad.emission.semantic = uint8_t(ar::SpectrumSemantic::REFLECTANCE);
    CHECK_FALSE(ar::light_valid(bad));  // colour must be an illuminant
    bad = pt;
    bad.emission.kind = uint8_t(ar::SpectrumKind::TABLE);
    CHECK_FALSE(ar::light_valid(bad));  // resolve() evaluates CONSTANT and RGB only
    bad = pt;
    bad.type = 200;
    CHECK_FALSE(ar::light_valid(bad));
  }

  AR_RT_CASE("lights_point/point lamp") {
    const Obs obs;
    const LightRecord lamp = make_point(r3(4, -1, 3));
    const ar::LightSampleContext ctx = ctx_at(r3(1, 2, 0.5));
    const LightLiSample s = ar::light_sample_li(lamp, ctx, r2(0.3, 0.7), lanes(), obs.view);
    // NumPy: d = |c - p|, wi = (c - p) / d, Le = P s / (4 pi) / d^2.
    const double wi[3] = {0.60920769908, -0.60920769908, 0.507673082567};
    const double le[4] = {0.484233016289, 0.566476743325, 0.607701155131, 0.689124495862};
    CHECK(rel_close(double(s.dist), 4.9244289009, kPinTol));
    CHECK(close(double(s.wi.x), wi[0], kPinTol));
    CHECK(close(double(s.wi.y), wi[1], kPinTol));
    CHECK(close(double(s.wi.z), wi[2], kPinTol));
    for (int i = 0; i < 4; ++i) CHECK(rel_close(double(s.Le.v[i]), le[i], kPinTol));
    CHECK(s.pdf == Real(1));
    CHECK(s.delta);
    CHECK_FALSE(s.infinite);
    CHECK(close(double(s.p_light.x), 4.0, kPinTol));
    // The result does not depend on u (deterministic in the context).
    const LightLiSample s2 = ar::light_sample_li(lamp, ctx, r2(0.9, 0.1), lanes(), obs.view);
    CHECK(s2.wi.x == s.wi.x);
    CHECK(s2.Le.v[2] == s.Le.v[2]);
    // A delta light has no BSDF-strategy density (D1).
    const ar::LightHit hit{r3(4, -1, 3), r3(0, 0, 1)};
    CHECK(ar::light_pdf_li(lamp, ctx, s.wi, hit) == Real(0));

    // Irradiance on a surface with normal n is exactly I cos / d^2, I = P s / (4 pi).
    const D3 n = unit3(D3{0.2, -0.3, 0.9});
    const double cosn = dot3(n, d3(s.wi));
    for (int i = 0; i < 4; ++i) {
      const double e = double(s.Le.v[i]) * cosn / double(s.pdf);
      const double d = double(s.dist);
      CHECK(rel_close(e, kStrength * illum_at(i) / (4.0 * kPi) * cosn / (d * d), kPinTol));
    }
    // On the lamp itself there is no direction: no sample, not an infinite Le.
    const LightLiSample on = ar::light_sample_li(lamp, ctx_at(r3(4, -1, 3)), r2(0.5, 0.5), lanes(), obs.view);
    CHECK(on.pdf == Real(0));
    CHECK(on.Le.v[0] == Real(0));
  }

  AR_RT_CASE("lights_point/point sample_le and bounds") {
    const Obs obs;
    const LightRecord lamp = make_point(r3(4, -1, 3));
    const ar::SceneSphere world{r3(0, 0, 0), Real(100)};
    // NumPy SampleUniformSphere((0.6, 0.2)); intensity P s / (4 pi).
    const LightLeSample s = ar::light_sample_le(lamp, r2(0.1, 0.9), r2(0.6, 0.2), lanes(), obs.view, world);
    CHECK(close(double(s.ray.o.x), 4.0, kPinTol));
    CHECK(close(double(s.ray.d.x), 0.302773583227, kPinTol));
    CHECK(close(double(s.ray.d.y), 0.931841272589, kPinTol));
    CHECK(close(double(s.ray.d.z), -0.2, kPinTol));
    CHECK(rel_close(double(s.pdf_dir), 0.0795774715459, kPinTol));
    CHECK(s.pdf_pos == Real(1));
    const double inten[4] = {11.742650645, 13.7370610256, 14.7367530119, 16.7112690246};
    for (int i = 0; i < 4; ++i) CHECK(rel_close(double(s.Le.v[i]), inten[i], kPinTol));
    const ar::LightBounds b = ar::light_bounds(lamp);
    CHECK(b.bbox.lo.y == Real(-1));
    CHECK(b.bbox.hi.y == Real(-1));
    CHECK(rel_close(double(b.theta_o), kPi, kPinTol));
    CHECK(rel_close(double(b.theta_e), kPi / 2, kPinTol));
    CHECK_FALSE(b.two_sided);
  }

  AR_RT_CASE("lights_point/sphere pinned samples") {
    const Obs obs;
    struct Row {
      double p[3], c[3], r, u0, u1;
      double wi[3], dist, pdf, pl[3];
    };
    // NumPy transcription of Sphere::Sample(ctx, u). Rows: 3.3 r; 1.1 r (wide cone); 303 r
    // (Taylor branch); u0 = 0 on the axis.
    const Row rows[] = {
        {{1, 2, 0.5}, {4, -1, 3}, 1.5, 0.3, 0.8, {0.60071903723, -0.492693563803, 0.62959486219}, 3.60382504317, 3.34915893135,
         {3.16488631028, 0.224418596159, 2.76894973141}},
        {{0, 0, 0}, {0, 0, 1.65}, 1.5, 0.9, 0.15, {-0.517262338537, -0.711950530903, 0.474938011405}, 0.407344985358,
         0.272804834275, {-0.210704219718, -0.290009478586, 0.193463617302}},
        {{-3, 5, -2}, {290, -60, 40}, 1.0, 0.55, 0.35, {0.9667379976, -0.216124784657, 0.136776172826}, 302.376123165,
         29232.9433273, {289.318487831, -60.3509745044, 39.3578488805}},
        {{0, 0, 0}, {0, 0, -30}, 2.0, 0.0, 0.5, {0, 0, -1}, 28.0, 71.5400583034, {0, 0, -28}}};
    for (const Row& row : rows) {
      const LightRecord lamp = make_sphere(r3(row.c[0], row.c[1], row.c[2]), row.r);
      const ar::LightSampleContext ctx = ctx_at(r3(row.p[0], row.p[1], row.p[2]));
      const LightLiSample s = ar::light_sample_li(lamp, ctx, r2(row.u0, row.u1), lanes(), obs.view);
      INFO("r=" << row.r << " u=(" << row.u0 << "," << row.u1 << ")");
      CHECK(close(double(s.wi.x), row.wi[0], kPinTol));
      CHECK(close(double(s.wi.y), row.wi[1], kPinTol));
      CHECK(close(double(s.wi.z), row.wi[2], kPinTol));
      CHECK(rel_close(double(s.dist), row.dist, kPinTol));
      CHECK(rel_close(double(s.pdf), row.pdf, kPinTol));
      CHECK(close(double(s.p_light.x), row.pl[0], kPinTol));
      CHECK(close(double(s.p_light.y), row.pl[1], kPinTol));
      CHECK(close(double(s.p_light.z), row.pl[2], kPinTol));
      CHECK_FALSE(s.delta);
      CHECK_FALSE(s.infinite);
    }
    // L = P s / (4 pi^2 r^2) at P = 250, r = 1.5 (NumPy).
    const double radiance[4] = {1.66124524014, 1.94339659181, 2.08482407731, 2.36416095165};
    const LightLiSample s = ar::light_sample_li(make_sphere(r3(4, -1, 3), 1.5), ctx_at(r3(1, 2, 0.5)), r2(0.3, 0.8), lanes(), obs.view);
    for (int i = 0; i < 4; ++i) CHECK(rel_close(double(s.Le.v[i]), radiance[i], kPinTol));
  }

  AR_RT_CASE("lights_point/sphere pdf consistency") {
    // pdf_li(sample.wi) == sample.pdf at 1.1 r to 1000 r, also against the closed-form cone
    // pdf. 30 r is skipped on purpose: near pbrt's 1.5 degree switch to the Taylor form its
    // pdf differs from the exact one by s/4 (1.7e-4), which is pbrt's choice, not a defect.
    const Obs obs;
    const double ratios[] = {1.1, 2.0, 5.0, 10.0, 100.0, 1000.0};
    const double radius = 0.75;
    const D3 axis = unit3(D3{0.3, -0.5, 0.8});
    int checked = 0;
    for (double ratio : ratios) {
      const double dist = ratio * radius;
      const Real3 p = r3(1.0, -2.0, 0.5);
      const Real3 c = r3(1.0 + dist * axis.x, -2.0 + dist * axis.y, 0.5 + dist * axis.z);
      const LightRecord lamp = make_sphere(c, radius);
      const ar::LightSampleContext ctx = ctx_at(p);
      const D3 cs = d3(ar::light_position(lamp));  // the stored (float) centre
      const double d_true = len3(sub3(cs, d3(p)));
      const double want = cone_pdf(radius, d_true);
      const ar::LightHit hit{c, r3(0, 0, 1)};
      for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) {
          const LightLiSample s = ar::light_sample_li(lamp, ctx, r2((i + 0.5) / 16, (j + 0.5) / 16), lanes(), obs.view);
          INFO("ratio=" << ratio << " i=" << i << " j=" << j);
          REQUIRE(s.pdf > Real(0));
          const double pdf_li = double(ar::light_pdf_li(lamp, ctx, s.wi, hit));
          CHECK(rel_close(pdf_li, double(s.pdf), 1e-4));
          CHECK(rel_close(double(s.pdf), want, 1e-4));
          // The sample lies on the sphere, on the side that faces the context point, and wi
          // points at it.
          const D3 pl = d3(s.p_light);
          // Rounding of p_light itself (magnitude |c|) bounds the float tolerances.
          CHECK(std::fabs(len3(sub3(pl, cs)) - radius) <= (kF32 ? 3e-7 : 1e-15) * (dist + 1.0));
          CHECK(dot3(sub3(pl, cs), sub3(d3(p), pl)) >= -(kF32 ? 1e-4 : 1e-12) * radius * d_true);
          const D3 to = sub3(pl, d3(p));
          CHECK(std::fabs(len3(to) - double(s.dist)) <= 1e-4 * double(s.dist));
          CHECK(dot3(unit3(to), d3(s.wi)) >= 1.0 - 1e-6);
          ++checked;
        }
    }
    CHECK(checked == 6 * 256);
    // Inside or on the sphere, and at the centre: no sample and a zero pdf.
    const LightRecord lamp = make_sphere(r3(0, 0, 0), 1.0);
    const ar::LightHit hit{r3(1, 0, 0), r3(1, 0, 0)};
    for (double d : {0.0, 0.5, 1.0}) {
      const ar::LightSampleContext in = ctx_at(r3(d, 0, 0));
      const LightLiSample s = ar::light_sample_li(lamp, in, r2(0.5, 0.5), lanes(), obs.view);
      CHECK(s.pdf == Real(0));
      CHECK(s.Le.v[1] == Real(0));
      CHECK(ar::light_pdf_li(lamp, in, r3(1, 0, 0), hit) == Real(0));
    }
  }

  AR_RT_CASE("lights_point/sphere irradiance") {
    // E from 2^16 cone samples (a 256 x 256 midpoint grid of the two uniforms, so the result is
    // deterministic) against E = P s cos(theta_n) / (4 pi d^2), within 0.5% (0004 V D6). The
    // normal tilts away from the axis by less than 90 degrees minus the cone half angle, so the
    // whole sphere is above the horizon.
    const Obs obs;
    struct Row {
      double ratio, tilt_deg;
    };
    const Row rows[] = {{1.1, 0}, {1.5, 30}, {3, 60}, {10, 80}, {50, 85}, {1000, 0}};
    const double radius = 2.0;
    const D3 axis = unit3(D3{-0.4, 0.6, 0.7});
    const D3 e1 = unit3(cross3(axis, D3{0, 0, 1}));
    constexpr int kG = 256;
    for (const Row& row : rows) {
      const double dist = row.ratio * radius;
      const double ta = row.tilt_deg * kPi / 180.0;
      const D3 n = D3{axis.x * std::cos(ta) + e1.x * std::sin(ta), axis.y * std::cos(ta) + e1.y * std::sin(ta),
                      axis.z * std::cos(ta) + e1.z * std::sin(ta)};
      const Real3 p = r3(3, -1, 2);
      const Real3 c = r3(3 + dist * axis.x, -1 + dist * axis.y, 2 + dist * axis.z);
      const LightRecord lamp = make_sphere(c, radius);
      const ar::LightSampleContext ctx = ctx_at(p);
      double acc[4] = {0, 0, 0, 0};
      for (int i = 0; i < kG; ++i)
        for (int j = 0; j < kG; ++j) {
          const LightLiSample s = ar::light_sample_li(lamp, ctx, r2((i + 0.5) / kG, (j + 0.5) / kG), lanes(), obs.view);
          const double cosn = std::fmax(0.0, dot3(n, d3(s.wi)));
          for (int l = 0; l < 4; ++l) acc[l] += double(s.Le.v[l]) * cosn / double(s.pdf);
        }
      const double cos_axis = std::cos(ta);
      for (int l = 0; l < 4; ++l) {
        const double got = acc[l] / (double(kG) * kG);
        const double want = kStrength * illum_at(l) / (4.0 * kPi) * cos_axis / (dist * dist);
        INFO("ratio=" << row.ratio << " tilt=" << row.tilt_deg << " lane " << l << " got " << got << " want " << want);
        CHECK(rel_close(got, want, 5e-3));
      }
    }
  }

  AR_RT_CASE("lights_point/power from radiance") {
    // The sphere's radiance from P and r reproduces P: flux = L * pi * (4 pi r^2) by midpoint
    // quadrature over the area (uniform) and the outward hemisphere, and by the Le sampling
    // route Le cos / (pdf_pos pdf_dir), which is constant per sample. 1e-4 (0004 V D6).
    const Obs obs;
    const double radii[] = {0.05, 1.0, 37.0};
    for (double radius : radii) {
      const LightRecord lamp = make_sphere(r3(1, 2, 3), radius);
      const Spec rad = ar::sphere_radiance(lamp, lanes(), obs.view);
      constexpr int kNz = 512;
      double hemi = 0.0;  // integral of cos over the hemisphere
      for (int i = 0; i < kNz; ++i) hemi += (i + 0.5) / kNz * (1.0 / kNz) * 2.0 * kPi;
      for (int l = 0; l < 4; ++l) {
        const double flux = double(rad.v[l]) * hemi * 4.0 * kPi * radius * radius;
        INFO("radius=" << radius << " lane " << l);
        CHECK(rel_close(flux, kStrength * illum_at(l), 1e-4));
      }
      Rng rng{st::mix_bits(uint64_t(radius * 1000) + 11)};
      const ar::SceneSphere world{r3(0, 0, 0), Real(100)};
      for (int k = 0; k < 200; ++k) {
        const Real2 up = r2(rng.next(), rng.next()), ud = r2(rng.next(), rng.next());
        const LightLeSample s = ar::light_sample_le(lamp, up, ud, lanes(), obs.view, world);
        if (!(s.pdf_dir > Real(0))) continue;
        const D3 nrm = unit3(sub3(d3(s.ray.o), D3{1, 2, 3}));
        const double cosd = dot3(nrm, d3(s.ray.d));
        for (int l = 0; l < 4; ++l) {
          const double flux = double(s.Le.v[l]) * cosd / (double(s.pdf_pos) * double(s.pdf_dir));
          CHECK(rel_close(flux, kStrength * illum_at(l), 2e-4));
        }
      }
    }
    const LightRecord pt = make_point(r3(1, 2, 3));
    const Spec inten = ar::point_intensity(pt, lanes(), obs.view);
    for (int l = 0; l < 4; ++l) CHECK(rel_close(double(inten.v[l]) * 4.0 * kPi, kStrength * illum_at(l), 1e-4));
  }

  AR_RT_CASE("lights_point/sphere sample_le and bounds") {
    const Obs obs;
    const LightRecord lamp = make_sphere(r3(4, -1, 3), 1.5);
    const ar::SceneSphere world{r3(0, 0, 0), Real(100)};
    // NumPy: area point from u_pos = (0.3, 0.8), cosine hemisphere about its normal from
    // u_dir = (0.6, 0.2) through the Duff frame.
    const LightLeSample s = ar::light_sample_le(lamp, r2(0.3, 0.8), r2(0.6, 0.2), lanes(), obs.view, world);
    CHECK(close(double(s.ray.o.x), 4.42482813033, kPinTol));
    CHECK(close(double(s.ray.o.y), -2.30748654283, kPinTol));
    CHECK(close(double(s.ray.o.z), 3.6, kPinTol));
    CHECK(close(double(s.ray.d.x), 0.270772770865, kPinTol));
    CHECK(close(double(s.ray.d.y), -0.934970526448, kPinTol));
    CHECK(close(double(s.ray.d.z), -0.229155452108, kPinTol));
    CHECK(rel_close(double(s.pdf_pos), 0.0353677651315, kPinTol));
    CHECK(rel_close(double(s.pdf_dir), 0.254647908947, kPinTol));
    CHECK(rel_close(double(s.Le.v[1]), 1.94339659181, kPinTol));
    const ar::LightBounds b = ar::light_bounds(lamp);
    CHECK(close(double(b.bbox.lo.x), 2.5, kPinTol));
    CHECK(close(double(b.bbox.hi.z), 4.5, kPinTol));
    CHECK(rel_close(double(b.theta_e), kPi / 2, kPinTol));
    CHECK_FALSE(b.two_sided);
  }

  AR_RT_CASE("lights_point/boundary uniforms") {
    // u = 0 and the largest Real below 1, in every combination: finite outputs, and pdf 0 means
    // no sample with Le 0, never a NaN. Includes a sphere narrowed to the radius-0 point limit.
    const Obs obs;
    const Real lo = Real(0), hi = std::nextafter(Real(1), Real(0));
    const Real vals[] = {lo, hi};
    const ar::SceneSphere world{r3(0, 0, 0), Real(100)};
    const double radius_list[] = {1.0, 1e-3, 1e-6, 1e-12, 1e-30, 0.0};
    const double dist_list[] = {1.0001, 1.1, 10.0, 1e4, 1e6, 0.0};
    for (double radius : radius_list)
      for (double dist : dist_list)
        for (Real a : vals)
          for (Real b : vals) {
            const LightRecord lamp = make_sphere(r3(0, 0, dist), radius);  // radius 0 is invalid to build, safe to run
            const ar::LightSampleContext ctx = ctx_at(r3(0, 0, 0));
            const LightLiSample s = ar::light_sample_li(lamp, ctx, Real2{a, b}, lanes(), obs.view);
            INFO("radius=" << radius << " dist=" << dist << " u=(" << double(a) << "," << double(b) << ")");
            CHECK(finite3(s.wi));
            CHECK(finite3(s.p_light));
            CHECK(std::isfinite(double(s.dist)));
            CHECK(std::isfinite(double(s.pdf)));
            CHECK(finite_spec(s.Le));
            if (s.pdf == Real(0)) {
              CHECK(s.Le.v[0] == Real(0));
            } else {
              CHECK(s.pdf > Real(0));
              CHECK(s.dist > Real(0));
            }
            const ar::LightHit hit{r3(0, 0, dist), r3(0, 0, 1)};
            const Real pdf = ar::light_pdf_li(lamp, ctx, r3(0, 0, 1), hit);
            CHECK(std::isfinite(double(pdf)));
            CHECK(pdf >= Real(0));
            const LightLeSample e = ar::light_sample_le(lamp, Real2{a, b}, Real2{b, a}, lanes(), obs.view, world);
            CHECK(finite3(e.ray.o));
            CHECK(finite3(e.ray.d));
            CHECK(finite_spec(e.Le));
            CHECK(std::isfinite(double(e.pdf_pos)));
            CHECK(std::isfinite(double(e.pdf_dir)));
            if (e.pdf_pos == Real(0)) CHECK(e.Le.v[0] == Real(0));
          }
    for (Real a : vals)
      for (Real b : vals) {
        const LightRecord lamp = make_point(r3(0, 0, 5));
        const LightLiSample s = ar::light_sample_li(lamp, ctx_at(r3(0, 0, 0)), Real2{a, b}, lanes(), obs.view);
        CHECK(s.pdf == Real(1));
        CHECK(finite_spec(s.Le));
        const LightLeSample e = ar::light_sample_le(lamp, Real2{a, b}, Real2{b, a}, lanes(), obs.view, world);
        CHECK(finite3(e.ray.d));
        CHECK(e.pdf_dir > Real(0));
        CHECK(finite_spec(e.Le));
      }
  }
}

TEST_SUITE("gate") {
  // Chi-squared of the sampled directions against the pdf: cells of equal cos(angle from the
  // axis to the centre) between cos(theta_max) and 1, times azimuth sectors. A cell's expected
  // count is n * pdf_li * (its solid angle (d cos) (d phi)), so the pdf under test sets the
  // expectation; the cells must also sum to n (pdf integrates to 1 over the cone).
  static void chi2_sphere(double ratio, const char* name) {
    const st::Run run = st::begin(name, st::gate_family());
    const Obs obs;
    const double radius = 1.5;
    const D3 axis = unit3(D3{0.3, -0.5, 0.8});
    const D3 e1 = unit3(cross3(axis, D3{0, 0, 1}));
    const D3 e2 = cross3(axis, e1);
    const double dist = ratio * radius;
    const Real3 p = r3(0, 0, 0);
    const Real3 c = r3(dist * axis.x, dist * axis.y, dist * axis.z);
    const LightRecord lamp = make_sphere(c, radius);
    const ar::LightSampleContext ctx = ctx_at(p);
    const ar::LightHit hit{c, r3(0, 0, 1)};
    const double pdf = double(ar::light_pdf_li(lamp, ctx, r3(0, 0, 1), hit));
    const double cos_max = std::sqrt(1.0 - (radius / dist) * (radius / dist));
    const uint64_t seed = st::mix_bits(run.seed ^ uint64_t(ratio * 100));
    auto gen = [&](int ires) {
      const int nb = ires, ns = 2 * ires;
      const int cells = nb * ns;
      const int n = 2000 * cells;
      st::Histogram h;
      h.observed.assign(cells, 0.0);
      h.expected.assign(cells, 0.0);
      const double dcos = (1.0 - cos_max) / nb, dphi = 2.0 * kPi / ns;
      for (int k = 0; k < cells; ++k) h.expected[k] = n * pdf * dcos * dphi;
      Rng rng{seed};
      int outside = 0;
      for (int k = 0; k < n; ++k) {
        const LightLiSample s = ar::light_sample_li(lamp, ctx, r2(rng.next(), rng.next()), lanes(), obs.view);
        const D3 w = unit3(d3(s.wi));
        const double ct = dot3(w, axis);
        double ph = std::atan2(dot3(w, e2), dot3(w, e1));
        if (ph < 0) ph += 2.0 * kPi;
        if (ct < cos_max - 1e-5) ++outside;
        const int b = std::min(nb - 1, std::max(0, int((ct - cos_max) / dcos)));
        const int sct = std::min(ns - 1, int(ph / dphi));
        h.observed[b * ns + sct] += 1.0;
      }
      CHECK(outside == 0);
      return h;
    };
    const st::Chi2Result res = st::chi2_with_retry(run, gen, 8);
    std::printf("lights_point chi2 d/r=%.1f: chi2 %.1f dof %d p %.4f%s\n", ratio, res.chi2, res.dof, res.p,
                res.retried ? " (retried at 16)" : "");
    CHECK_MESSAGE(res.pass, res.message);
    CHECK(rel_close(pdf * (2.0 * kPi * (1.0 - cos_max)), 1.0, 1e-4));
  }

  AR_RT_CASE("lights_point/sphere chi2 1.2 r") { chi2_sphere(1.2, "lights_point/sphere chi2 1.2 r" AR_RT_SUFFIX); }
  AR_RT_CASE("lights_point/sphere chi2 3 r") { chi2_sphere(3.0, "lights_point/sphere chi2 3 r" AR_RT_SUFFIX); }
  AR_RT_CASE("lights_point/sphere chi2 100 r") { chi2_sphere(100.0, "lights_point/sphere chi2 100 r" AR_RT_SUFFIX); }
}
