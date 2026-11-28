// SPDX-License-Identifier: Apache-2.0
// ar-f64: compiled twice into this executable, once with AR_REAL_DOUBLE (cmake/sources.cmake).
// The light tree of 0004 D10 (kernel/light/light_tree.h, host/light/light_tree.h): node layout,
// builder shape, pmf consistency (D7 row: pmf(sampled) == pmf to 1e-4), pmf 0 for importance 0
// (D9), finite importance inside cluster bounds (D10 tree row), and the alias table as the tree's
// oracle: both estimate sum_i f_i(p), f_i = P_i max(0, cos) / d^2 written out here, and must agree
// within 3 standard errors (D1's criterion; Sidak over the gate family in the gate).
// The alias table is the host LightTable, built only in the float copy (its builder is compiled
// for the float Real); every other case runs in both copies.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "common/stat.h"
#include "host/light/light_tree.h"
#include "light/light.h"
#include "light/light_tree.h"
#if !defined(AR_REAL_DOUBLE)
#include "host/data_resource.h"
#include "host/light/light_builder.h"
#endif

namespace st = astroray::stat;

namespace {

using ar::LightRecord;
using ar::Real;
using ar::Real3;

constexpr double kPi = 3.14159265358979323846;
constexpr bool kF32 = sizeof(Real) == 4;
const ar::SceneSphere kScene{Real3{Real(5), Real(5), Real(5)}, Real(12)};

Real3 r3(double x, double y, double z) { return Real3{Real(x), Real(y), Real(z)}; }

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
  uint32_t bits() { return uint32_t(next() * 4294967296.0); }
};

ar::SpectrumRef white() { return ar::make_const_ref(ar::SpectrumSemantic::ILLUMINANT, Real(1)); }

// The scene: lights at z in [1, 10] that all emit downward or everywhere, so from below (shading
// points at z <= -2 with normals within 5 degrees of +z) every light is in front of the surface
// and inside its own emission cone, and no descent can end at two invisible children.
struct Lights {
  std::vector<LightRecord> rec;
  std::vector<double> imp;     // P times the type's geometric factor (light_power)
  std::vector<uint32_t> zero;  // lights of importance 0
  uint32_t world = ar::kNoLight;
  uint32_t one_sided = ar::kNoLight;  // a one-sided triangle facing -z
};

void push(Lights& L, const LightRecord& r, double strength) {
  L.rec.push_back(r);
  L.imp.push_back(double(ar::light_power(r, kScene, Real(1))) * strength);
}

// 64 local lights cycling point, sphere, rect, disk, one-sided triangle (wound down), two-sided
// triangle (wound up: its axis points away from the shading points), back-face triangle (wound
// up, emits down); then optionally the world and two triangles of importance 0.
Lights mixed(bool world, bool zeros) {
  Rng rng{154};
  auto u = [&](double a, double b) { return a + (b - a) * rng.next(); };
  Lights L;
  for (int i = 0; i < 64; ++i) {
    const Real3 p = r3(u(0, 10), u(0, 10), u(1, 10));
    const double s = u(1, 100), a = u(0.2, 1), b = u(0.2, 1);
    switch (i % 7) {
      case 0: push(L, ar::light_make(ar::LightType::POINT, p, Real(0), Real(s), white()), s); break;
      case 1: push(L, ar::light_make(ar::LightType::SPHERE, p, Real(u(0.05, 0.4)), Real(s), white()), s); break;
      case 2: push(L, ar::light_make_area(ar::LightType::AREA_RECT, p, r3(1, 0, 0), r3(0, 1, 0), Real(a), Real(b), Real(s), white()), s); break;
      case 3: push(L, ar::light_make_area(ar::LightType::AREA_DISK, p, r3(1, 0, 0), r3(0, 1, 0), Real(a), Real(0), Real(s), white()), s); break;
      case 4:
        if (L.one_sided == ar::kNoLight) L.one_sided = uint32_t(L.rec.size());
        push(L, ar::light_make_triangle(p, p + r3(0, b, 0), p + r3(a, 0, 0), 1, uint32_t(i), 0, Real(s), white()), s);
        break;
      case 5: push(L, ar::light_make_triangle(p, p + r3(a, 0, 0), p + r3(0, b, 0), 1, uint32_t(i), ar::kLightTwoSided, Real(s), white()), s); break;
      default: push(L, ar::light_make_triangle(p, p + r3(a, 0, 0), p + r3(0, b, 0), 1, uint32_t(i), ar::kLightBackFace, Real(s), white()), s);
    }
  }
  if (world) {
    L.world = uint32_t(L.rec.size());
    push(L, ar::light_make_world(Real(0.02), white()), 0.02);
  }
  if (zeros)
    for (int k = 0; k < 2; ++k) {
      const Real3 p = r3(2 + 3 * k, 4, 3);
      L.zero.push_back(uint32_t(L.rec.size()));
      push(L, ar::light_make_triangle(p, p + r3(0, 1, 0), p + r3(1, 0, 0), 2, uint32_t(k), ar::kLightEmissionProgram, Real(0), white()), 0.0);
    }
  return L;
}

struct Shade {
  Real3 p, n;
};

// Below every light, normal within 5 degrees of +z.
Shade shade_point(Rng& rng) {
  const double ct = 1.0 - rng.next() * (1.0 - std::cos(5.0 * kPi / 180.0)), st_ = std::sqrt(1.0 - ct * ct);
  const double ph = 2.0 * kPi * rng.next();
  return Shade{r3(10 * rng.next(), 10 * rng.next(), -2 - 8 * rng.next()), r3(st_ * std::cos(ph), st_ * std::sin(ph), ct)};
}

// f_i(p) = P_i max(0, cos theta_surface) / d^2 toward the light's centre, 0 when p is behind a
// one-sided emitter; the world (no distance) contributes its importance / 100. Written from the
// light records here, not from the code under test.
double f_light(const Lights& L, uint32_t i, const Shade& s) {
  const LightRecord& r = L.rec[i];
  if (i == L.world) return L.imp[i] / 100.0;
  double c[3], front[3] = {0, 0, 0};
  bool one_sided = false;
  if (r.type == uint8_t(ar::LightType::TRIANGLE)) {
    double e1[3], e2[3];
    for (int k = 0; k < 3; ++k) {
      c[k] = (double(r.xf[k]) + r.xf[3 + k] + r.xf[6 + k]) / 3.0;
      e1[k] = double(r.xf[3 + k]) - r.xf[k];
      e2[k] = double(r.xf[6 + k]) - r.xf[k];
    }
    const double sign = (r.flags & ar::kLightBackFace) ? -1.0 : 1.0;
    front[0] = sign * (e1[1] * e2[2] - e1[2] * e2[1]);
    front[1] = sign * (e1[2] * e2[0] - e1[0] * e2[2]);
    front[2] = sign * (e1[0] * e2[1] - e1[1] * e2[0]);
    one_sided = !(r.flags & ar::kLightTwoSided);
  } else {
    c[0] = r.xf[3], c[1] = r.xf[7], c[2] = r.xf[11];
    if (r.type == uint8_t(ar::LightType::AREA_RECT) || r.type == uint8_t(ar::LightType::AREA_DISK)) {
      front[0] = -double(r.xf[2]), front[1] = -double(r.xf[6]), front[2] = -double(r.xf[10]);  // local -Z
      one_sided = true;
    }
  }
  const double d[3] = {c[0] - double(s.p.x), c[1] - double(s.p.y), c[2] - double(s.p.z)};
  const double d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
  if (one_sided && front[0] * d[0] + front[1] * d[1] + front[2] * d[2] >= 0.0) return 0.0;
  const double cs = (double(s.n.x) * d[0] + double(s.n.y) * d[1] + double(s.n.z) * d[2]) / std::sqrt(d2);
  return L.imp[i] * std::max(0.0, cs) / d2;
}

// Ruling 90: a node's energy is the light's on-axis intensity, written out from the importance
// (a power): P / (4 pi) for point and sphere lamps, P / pi for area lamps, L A = (pi A L) / pi for a
// triangle, pi L = (4 pi^2 R^2 L) / (4 pi R^2) for the world.
double intensity(const Lights& L, uint32_t i) {
  const ar::LightType t = ar::LightType(L.rec[i].type);
  if (t == ar::LightType::POINT || t == ar::LightType::SPHERE) return L.imp[i] / (4.0 * kPi);
  if (t == ar::LightType::WORLD_CONST) return L.imp[i] / (4.0 * kPi * 12.0 * 12.0);
  return L.imp[i] / kPi;
}

int ceil_log2(size_t n) {
  int k = 0;
  while ((size_t(1) << k) < n) ++k;
  return k;
}

double angle(const float* a, const float* b) {
  const double c = double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
  return std::acos(std::clamp(c, -1.0, 1.0));
}

}  // namespace

TEST_SUITE("fast") {

AR_RT_CASE("light_tree/node size") {
  CHECK(sizeof(ar::LightTreeNode) == 64);
  CHECK(sizeof(ar::LightTreeEmitter) == 8);
  CHECK(alignof(ar::LightTreeNode) == 4);
}

AR_RT_CASE("light_tree/builder shape") {
  for (bool world : {false, true}) {
    INFO("world " << world);
    const Lights L = mixed(world, true);
    const astroray::host::LightTree t = astroray::host::build_light_tree(L.rec, L.imp, kScene);
    const ar::LightTreeView v = t.view();
    size_t positive = 0;
    double sum = 0.0;
    for (uint32_t i = 0; i < L.rec.size(); ++i) positive += L.imp[i] > 0.0, sum += intensity(L, i);
    size_t leaves = 0;
    for (const ar::LightTreeNode& n : t.nodes) leaves += (n.flags & ar::kLightTreeLeaf) != 0;
    CHECK(leaves == positive);
    CHECK(t.nodes.size() == 2 * positive - 1);
    CHECK(double(t.nodes[0].energy) == doctest::Approx(sum).epsilon(1e-6));
    CHECK(v.emitter_count == L.rec.size());
    // 64 local lights: depth at most ceil(log2 64); the world adds the root's local/distant split.
    const uint32_t max_depth = uint32_t(ceil_log2(64) + (world ? 1 : 0));
    for (uint32_t i = 0; i < v.emitter_count; ++i) {
      const ar::LightTreeEmitter e = t.emitters[i];
      if (L.imp[i] == 0.0) {
        CHECK(e.depth == ar::kLightTreeAbsent);
        continue;
      }
      REQUIRE(e.depth <= max_depth);
      uint32_t idx = 0, trail = e.bit_trail;
      for (uint32_t d = 0; d < e.depth; ++d, trail >>= 1) {
        REQUIRE((t.nodes[idx].flags & ar::kLightTreeLeaf) == 0);
        idx = t.nodes[idx].child[trail & 1u];
      }
      REQUIRE((t.nodes[idx].flags & ar::kLightTreeLeaf) != 0);
      CHECK(t.nodes[idx].light == i);
      CHECK(double(t.nodes[idx].energy) == doctest::Approx(intensity(L, i)).epsilon(1e-6));
      CHECK(((t.nodes[idx].flags & ar::kLightTreeDistant) != 0) == (i == L.world));
    }
    if (world) {
      CHECK(t.emitters[L.world].depth == 1);
      CHECK(t.emitters[L.world].bit_trail == 1);
    }
    // Every inner node bounds its children: box, cone (Cycles' 5e-4 rad slack), spread, energy.
    for (const ar::LightTreeNode& n : t.nodes) {
      if (n.flags & ar::kLightTreeLeaf) continue;
      double e = 0.0;
      for (uint32_t c : n.child) {
        const ar::LightTreeNode& k = t.nodes[c];
        e += double(k.energy);
        if (!(k.flags & ar::kLightTreeDistant))
          for (int a = 0; a < 3; ++a) {
            CHECK(n.bbox_lo[a] <= k.bbox_lo[a]);
            CHECK(n.bbox_hi[a] >= k.bbox_hi[a]);
          }
        CHECK(angle(n.axis, k.axis) + double(k.theta_o) <= double(n.theta_o) + 1e-3);
        CHECK(k.theta_e <= n.theta_e);
      }
      CHECK(double(n.energy) == doctest::Approx(e).epsilon(1e-6));
      CHECK(double(n.axis[0]) * n.axis[0] + double(n.axis[1]) * n.axis[1] + double(n.axis[2]) * n.axis[2] ==
            doctest::Approx(1.0).epsilon(1e-6));
    }
  }
  // Deterministic: the same input gives the same bytes.
  const Lights L = mixed(true, true);
  const astroray::host::LightTree a = astroray::host::build_light_tree(L.rec, L.imp, kScene), b = astroray::host::build_light_tree(L.rec, L.imp, kScene);
  REQUIRE(a.nodes.size() == b.nodes.size());
  CHECK(std::equal(a.nodes.begin(), a.nodes.end(), b.nodes.begin(),
                   [](const ar::LightTreeNode& x, const ar::LightTreeNode& y) { return std::memcmp(&x, &y, sizeof x) == 0; }));
}

AR_RT_CASE("light_tree/builder refuses bad input") {
  CHECK_THROWS_AS(astroray::host::build_light_tree(std::vector<LightRecord>{}, std::vector<double>{}, kScene), std::runtime_error);
  const Lights L = mixed(false, false);
  CHECK_THROWS_AS(astroray::host::build_light_tree(L.rec, std::vector<double>(3, 1.0), kScene), std::runtime_error);
  std::vector<double> bad = L.imp;
  bad[3] = -1.0;
  CHECK_THROWS_AS(astroray::host::build_light_tree(L.rec, bad, kScene), std::runtime_error);
  bad[3] = std::nan("");
  CHECK_THROWS_AS(astroray::host::build_light_tree(L.rec, bad, kScene), std::runtime_error);
  bad[3] = 1e300;
  CHECK_THROWS_AS(astroray::host::build_light_tree(L.rec, bad, kScene), std::runtime_error);
  // Only lights of importance 0: no node, and nothing to draw.
  const std::vector<double> none(L.rec.size(), 0.0);
  const astroray::host::LightTree t = astroray::host::build_light_tree(L.rec, none, kScene);
  CHECK(t.nodes.empty());
  const ar::SampledLight s = ar::light_tree_sample(t.view(), r3(5, 5, -3), r3(0, 0, 1), Real(0.4));
  CHECK(s.light == ar::kNoLight);
  CHECK(s.pmf == Real(0));
  CHECK(ar::light_tree_pmf(t.view(), r3(5, 5, -3), r3(0, 0, 1), 0) == Real(0));
}

AR_RT_CASE("light_tree/pmf sums to one") {
  const Lights L = mixed(true, true);
  const astroray::host::LightTree t = astroray::host::build_light_tree(L.rec, L.imp, kScene);
  const ar::LightTreeView v = t.view();
  const double tol = kF32 ? 1e-5 : 1e-12;
  Rng rng{1541};
  double worst = 0.0;
  for (int k = 0; k < 200; ++k) {
    const Shade s = shade_point(rng);
    double sum = 0.0;
    for (uint32_t i = 0; i < v.emitter_count; ++i) {
      const double pmf = double(ar::light_tree_pmf(v, s.p, s.n, i));
      REQUIRE(std::isfinite(pmf));
      REQUIRE(pmf >= 0.0);
      sum += pmf;
    }
    worst = std::max(worst, std::fabs(sum - 1.0));
  }
  INFO("worst |sum - 1| = " << worst);
  CHECK(worst <= tol);
  CHECK(ar::light_tree_pmf(v, r3(5, 5, -3), r3(0, 0, 1), v.emitter_count) == Real(0));
}

AR_RT_CASE("light_tree/pmf matches sample") {
  const Lights L = mixed(true, true);
  const astroray::host::LightTree t = astroray::host::build_light_tree(L.rec, L.imp, kScene);
  const ar::LightTreeView v = t.view();
  Rng rng{1542};
  int drawn = 0;
  for (int k = 0; k < 10; ++k) {
    const Shade s = shade_point(rng);
    for (int j = 0; j < 1000; ++j) {
      const ar::SampledLight sl = ar::light_tree_sample(v, s.p, s.n, Real(rng.next()));
      REQUIRE(sl.light < v.emitter_count);
      REQUIRE(sl.pmf > Real(0));
      const double replay = double(ar::light_tree_pmf(v, s.p, s.n, sl.light));
      CHECK(std::fabs(replay - double(sl.pmf)) <= 1e-4 * double(sl.pmf));
      ++drawn;
    }
  }
  CHECK(drawn == 10000);
  // u at both ends of [0, 1) still gives a light.
  const Shade s{r3(5, 5, -3), r3(0, 0, 1)};
  for (Real u : {Real(0), std::nextafter(Real(1), Real(0))}) {
    const ar::SampledLight sl = ar::light_tree_sample(v, s.p, s.n, u);
    REQUIRE(sl.light < v.emitter_count);
    CHECK(double(ar::light_tree_pmf(v, s.p, s.n, sl.light)) == doctest::Approx(double(sl.pmf)).epsilon(1e-4).scale(0));
  }
}

AR_RT_CASE("light_tree/zero importance never drawn") {
  const Lights L = mixed(true, true);
  REQUIRE(L.zero.size() == 2);
  const astroray::host::LightTree t = astroray::host::build_light_tree(L.rec, L.imp, kScene);
  const ar::LightTreeView v = t.view();
  Rng rng{1543};
  int hits = 0;
  for (int k = 0; k < 20; ++k) {
    const Shade s = shade_point(rng);
    for (uint32_t z : L.zero) CHECK(ar::light_tree_pmf(v, s.p, s.n, z) == Real(0));
    for (int j = 0; j < 5000; ++j) {
      const ar::SampledLight sl = ar::light_tree_sample(v, s.p, s.n, Real(rng.next()));
      hits += std::count(L.zero.begin(), L.zero.end(), sl.light) != 0;
    }
  }
  CHECK(hits == 0);
}

AR_RT_CASE("light_tree/back of a one-sided triangle has pmf 0") {
  const Lights L = mixed(true, false);
  const astroray::host::LightTree t = astroray::host::build_light_tree(L.rec, L.imp, kScene);
  // Above the down-facing triangle, looking down at its back.
  const LightRecord& tri = L.rec[L.one_sided];
  const Real3 c = r3((double(tri.xf[0]) + tri.xf[3] + tri.xf[6]) / 3, (double(tri.xf[1]) + tri.xf[4] + tri.xf[7]) / 3, double(tri.xf[2]));
  const Real3 p = c + r3(0, 0, 5), n = r3(0, 0, -1);
  CHECK(ar::light_tree_pmf(t.view(), p, n, L.one_sided) == Real(0));
  // From below it is lit, and a lone triangle (the root is its leaf) follows the same rule.
  CHECK(ar::light_tree_pmf(t.view(), c - r3(0, 0, 5), -n, L.one_sided) > Real(0));
  const astroray::host::LightTree lone = astroray::host::build_light_tree(std::vector<LightRecord>{tri}, std::vector<double>{1.0}, kScene);
  REQUIRE(lone.nodes.size() == 1);
  CHECK(ar::light_tree_pmf(lone.view(), p, n, 0) == Real(0));
  const ar::SampledLight back = ar::light_tree_sample(lone.view(), p, n, Real(0.5));
  CHECK(back.light == ar::kNoLight);
  CHECK(back.pmf == Real(0));
  const ar::SampledLight front = ar::light_tree_sample(lone.view(), c - r3(0, 0, 5), -n, Real(0.5));
  CHECK(front.light == 0);
  CHECK(front.pmf == Real(1));
}

AR_RT_CASE("light_tree/finite positive importance inside node bounds") {
  const Lights L = mixed(true, false);
  const astroray::host::LightTree t = astroray::host::build_light_tree(L.rec, L.imp, kScene);
  Rng rng{1544};
  int tested = 0;
  for (const ar::LightTreeNode& node : t.nodes) {
    if (node.flags & ar::kLightTreeDistant) continue;
    bool solid = true;
    for (int a = 0; a < 3; ++a) solid = solid && node.bbox_hi[a] > node.bbox_lo[a];
    if (!solid) continue;  // a flat or point box has no interior
    for (int k = 0; k < 9; ++k) {
      double q[3], nd[3], nl = 0.0;
      for (int a = 0; a < 3; ++a) {
        const double f = k == 0 ? 0.5 : 0.01 + 0.98 * rng.next();  // the centre, then strictly inside
        q[a] = node.bbox_lo[a] + f * (double(node.bbox_hi[a]) - node.bbox_lo[a]);
        nd[a] = 2.0 * rng.next() - 1.0;
        nl += nd[a] * nd[a];
      }
      nl = std::sqrt(nl);
      const ar::LightTreeImportance imp = ar::light_tree_importance(node, r3(q[0], q[1], q[2]), r3(nd[0] / nl, nd[1] / nl, nd[2] / nl));
      INFO("node energy " << node.energy << " point " << q[0] << ", " << q[1] << ", " << q[2]);
      CHECK(std::isfinite(double(imp.max)));
      CHECK(imp.max > Real(0));
      CHECK(std::isfinite(double(imp.min)));
      CHECK(imp.min >= Real(0));
      ++tested;
    }
  }
  CHECK(tested >= 9 * 20);
  // On a point lamp itself (a box without extent) the descent still gives a finite pmf.
  const LightRecord pt = L.rec[0];
  REQUIRE(pt.type == uint8_t(ar::LightType::POINT));
  const Real3 at = ar::light_position(pt);
  const ar::SampledLight s = ar::light_tree_sample(t.view(), at, r3(0, 0, 1), Real(0.3));
  CHECK(std::isfinite(double(s.pmf)));
  CHECK(std::isfinite(double(ar::light_tree_pmf(t.view(), at, r3(0, 0, 1), 0))));
}

AR_RT_CASE("light_tree/importance pinned by hand") {
  // Cycles' node importance written with angles (theta_i, theta_u, theta, theta_o, theta_e of
  // Conty Estevez and Kulla 2018, Eq. 3), not with the sin/cos products of the code under test.
  const double tol = kF32 ? 2e-5 : 1e-12;
  {
    // A point lamp's leaf (box without extent, theta_o = pi) of energy E at distance d along n:
    // max = E / d^2, min = 0 (theta_e - theta_o < 0).
    ar::LightTreeNode node{};
    node.axis[2] = 1.0f;
    node.theta_o = float(kPi);
    node.theta_e = float(kPi / 2);
    node.energy = 7.0f;
    node.flags = ar::kLightTreeLeaf;
    const ar::LightTreeImportance imp = ar::light_tree_importance(node, r3(0, 0, -3), r3(0, 0, 1));
    CHECK(double(imp.max) == doctest::Approx(7.0 / 9.0).epsilon(tol));
    CHECK(imp.min == Real(0));
  }
  {
    // A disk of radius 0.5 facing -z (box 1 x 1 x 0, bounding radius r = sqrt(0.5)), seen from
    // below and off axis by a tilted surface.
    ar::LightTreeNode node{};
    node.bbox_lo[0] = node.bbox_lo[1] = -0.5f;
    node.bbox_hi[0] = node.bbox_hi[1] = 0.5f;
    node.axis[2] = -1.0f;
    node.theta_o = 0.0f;
    node.theta_e = float(kPi / 2);
    node.energy = 5.0f;
    node.flags = ar::kLightTreeLeaf;
    const double p[3] = {1.0, 0.3, -4.0};
    double n[3] = {0.3, -0.2, 1.0};
    const double nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (double& x : n) x /= nl;
    const double v[3] = {-p[0], -p[1], -p[2]};
    const double d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const double theta_u = std::asin(std::sqrt(0.5) / d);
    const double theta_i = std::acos((v[0] * n[0] + v[1] * n[1] + v[2] * n[2]) / d);
    const double theta = std::acos(v[2] / d);  // axis (0, 0, -1) against p - c = -v
    REQUIRE(theta_i > theta_u);                 // the incidence factor is below 1
    REQUIRE(theta > theta_u);                   // and so is the outgoing one
    const double want_max = 5.0 * std::cos(theta_i - theta_u) * std::cos(theta - theta_u) / (d * d);
    REQUIRE(theta + theta_u < kPi / 2);
    const double want_min = 5.0 * std::cos(theta_i + theta_u) * std::cos(theta + theta_u) / (d * d);
    const ar::LightTreeImportance imp = ar::light_tree_importance(node, r3(p[0], p[1], p[2]), r3(n[0], n[1], n[2]));
    CHECK(double(imp.max) == doctest::Approx(want_max).epsilon(tol));
    CHECK(double(imp.min) == doctest::Approx(want_min).epsilon(tol));
  }
}

AR_RT_CASE("light_tree/two-sided triangle seen from behind far away") {
  // Wound up (axis +z, theta_o 0, theta_e pi), seen from 1e4 below: the subtended angle rounds
  // to 0 in float, and the emitter still lights the point.
  const LightRecord tri = ar::light_make_triangle(r3(0, 0, 0), r3(1, 0, 0), r3(0, 1, 0), 0, 0, ar::kLightTwoSided, Real(10), white());
  const astroray::host::LightTree t = astroray::host::build_light_tree(std::vector<LightRecord>{tri}, std::vector<double>{1.0}, kScene);
  const Real3 p = r3(1.0 / 3, 1.0 / 3, -1e4), n = r3(0, 0, 1);
  CHECK(ar::light_tree_pmf(t.view(), p, n, 0) == Real(1));
  const ar::SampledLight s = ar::light_tree_sample(t.view(), p, n, Real(0.5));
  CHECK(s.light == 0);
  CHECK(s.pmf == Real(1));
}

AR_RT_CASE("light_tree/world bounds and pmf") {
  // Cycles' background cone: axis (0, 0, 1), theta_o = pi, theta_e = 0.
  const ar::LightBounds b = ar::light_bounds(ar::light_make_world(Real(1), white()));
  CHECK(b.w.x == Real(0));
  CHECK(b.w.y == Real(0));
  CHECK(b.w.z == Real(1));
  CHECK(double(b.theta_o) == doctest::Approx(kPi).epsilon(1e-6));
  CHECK(b.theta_e == Real(0));
  const Lights L = mixed(true, true);
  const astroray::host::LightTree t = astroray::host::build_light_tree(L.rec, L.imp, kScene);
  CHECK(ar::light_tree_pmf(t.view(), r3(5, 5, -3), r3(0, 0, 1), L.world) > Real(0));
  // A world alone is the whole tree.
  const astroray::host::LightTree w = astroray::host::build_light_tree(std::vector<LightRecord>{L.rec[L.world]},
                                                                       std::vector<double>{L.imp[L.world]}, kScene);
  REQUIRE(w.nodes.size() == 1);
  CHECK((w.nodes[0].flags & ar::kLightTreeDistant) != 0);
  CHECK(ar::light_tree_pmf(w.view(), r3(5, 5, -3), r3(0, 0, 1), 0) == Real(1));
}

}  // TEST_SUITE("fast")

#if !defined(AR_REAL_DOUBLE)
namespace {

const ar::ObserverView& observer() {
  static astroray::ObserverTables t;
  static ar::ObserverView v = [] {
    astroray::DataBlob b;
    REQUIRE(astroray::find_data("spectral/observer.bin", &b));
    REQUIRE(astroray::parse_observer(b.data, b.size, &t).empty());
    return ar::ObserverView{&t.cmf[0][0], t.d65};
  }();
  return v;
}

// The scene's LightTable (alias table and importances from the display weighting) and the tree
// built from that table's lights and importances, as the engine will build it.
struct Oracle {
  Lights L;
  std::shared_ptr<const astroray::host::LightTable> table;
  astroray::host::LightTree tree;
};

Oracle oracle() {
  Oracle o;
  o.L = mixed(true, true);
  std::vector<astroray::host::LightInput> in;
  for (const LightRecord& r : o.L.rec) in.push_back(astroray::host::LightInput{r, {}});
  o.table = astroray::host::build_light_table(in, kScene, observer(), nullptr);
  o.tree = astroray::host::build_light_tree(o.table->lights, o.table->importance, o.table->scene);
  return o;
}

struct Moments {
  double sum = 0.0, sum2 = 0.0;
  void add(double x) { sum += x, sum2 += x * x; }
  void add(const Moments& m) { sum += m.sum, sum2 += m.sum2; }
};

// Both estimates of sum_i f_i(p) over n draws, in `chunks` independent streams seeded from
// `seed`, run on up to hardware_concurrency threads; the chunk partition and its seeds do not
// depend on the thread count, so the result does not either. The tree's estimate divides by
// light_tree_pmf of the drawn light (its agreement with the returned pmf is its own case).
struct Estimate {
  double tree_mean, tree_se, alias_mean, alias_se, z;
};
Estimate estimate(const Oracle& o, const Shade& s, uint64_t seed, uint32_t n, uint32_t chunks) {
  const ar::LightTreeView tv = o.tree.view();
  const ar::LightSamplerView av = o.table->view();
  const ar::LightSampleContext ctx = ar::light_context_point(s.p, s.n, s.n);
  std::vector<Moments> tree(chunks), alias(chunks);
  auto run = [&](uint32_t c) {
    Rng rng{st::mix_bits(seed + c)};
    for (uint32_t k = 0; k < n / chunks; ++k) {
      const ar::SampledLight a = ar::light_sampler_sample(av, ctx, rng.bits(), Real(rng.next()), true);
      alias[c].add(a.light == ar::kNoLight ? 0.0 : f_light(o.L, a.light, s) / double(a.pmf));
      const ar::SampledLight t = ar::light_tree_sample(tv, s.p, s.n, Real(rng.next()));
      const double pmf = t.light == ar::kNoLight ? 0.0 : double(ar::light_tree_pmf(tv, s.p, s.n, t.light));
      tree[c].add(pmf > 0.0 ? f_light(o.L, t.light, s) / pmf : 0.0);
    }
  };
  const uint32_t threads = std::max(1u, std::min(chunks, std::thread::hardware_concurrency()));
  std::vector<std::thread> pool;
  for (uint32_t w = 0; w < threads; ++w)
    pool.emplace_back([&, w] {
      for (uint32_t c = w; c < chunks; c += threads) run(c);
    });
  for (std::thread& th : pool) th.join();
  Moments mt, ma;
  for (uint32_t c = 0; c < chunks; ++c) mt.add(tree[c]), ma.add(alias[c]);
  const double m = double(n / chunks) * chunks;
  Estimate e;
  e.tree_mean = mt.sum / m;
  e.alias_mean = ma.sum / m;
  e.tree_se = std::sqrt(std::max(0.0, mt.sum2 / m - e.tree_mean * e.tree_mean) / (m - 1.0));
  e.alias_se = std::sqrt(std::max(0.0, ma.sum2 / m - e.alias_mean * e.alias_mean) / (m - 1.0));
  e.z = std::fabs(e.tree_mean - e.alias_mean) / std::sqrt(e.tree_se * e.tree_se + e.alias_se * e.alias_se);
  return e;
}

double exact(const Oracle& o, const Shade& s) {
  double sum = 0.0;
  for (uint32_t i = 0; i < o.L.rec.size(); ++i) sum += f_light(o.L, i, s);
  return sum;
}

}  // namespace

TEST_SUITE("fast") {
TEST_CASE("light_tree/tree and alias estimates agree") {
  const Oracle o = oracle();
  Rng rng{1545};
  for (int k = 0; k < 4; ++k) {
    const Shade s = shade_point(rng);
    const Estimate e = estimate(o, s, 1545u + uint64_t(k), 1u << 14, 4);
    INFO("point " << k << ": tree " << e.tree_mean << " +- " << e.tree_se << ", alias " << e.alias_mean << " +- " << e.alias_se
                  << ", exact " << exact(o, s));
    CHECK(e.z < 3.0);
  }
}
}  // TEST_SUITE("fast")

TEST_SUITE("gate") {
TEST_CASE("light_tree/estimates agree 2^20 draws") {
  const st::Run run = st::begin("light_tree/estimates agree 2^20 draws");
  const Oracle o = oracle();
  Rng rng{run.seed};
  for (int k = 0; k < 3; ++k) {
    const Shade s = shade_point(rng);
    run.family->note_check();
    const Estimate e = estimate(o, s, st::mix_bits(run.seed + uint64_t(k)), 1u << 20, 64);
    std::printf("light_tree point %d: tree %.6g +- %.3g, alias %.6g +- %.3g, exact %.6g, z %.2f (limit %.2f)\n", k, e.tree_mean,
                e.tree_se, e.alias_mean, e.alias_se, exact(o, s), e.z, run.family->z());
    INFO(st::reproduce(run));
    CHECK(e.z < run.family->z());
    // The tree against the exact sum as well, so a bias shared with the alias table shows.
    run.family->note_check();
    CHECK(std::fabs(e.tree_mean - exact(o, s)) / e.tree_se < run.family->z());
  }
}
}  // TEST_SUITE("gate")
#endif
