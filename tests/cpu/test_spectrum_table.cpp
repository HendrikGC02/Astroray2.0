// SPDX-License-Identifier: Apache-2.0
// SpectrumRef kind TABLE (0005 item 9): the kernel lookup table_spectrum and resolve() with the
// spectrum arena, the SPECTRUM_TABLE graph op and its refusals, the compiler's arena (deduplicated by
// content, SPECTRUM_OUT_OF_RANGE and clamp entries once per material), and the scene arena the store
// builds with the offsets it relocates.
// Expected values are typed from the setups below or computed in double from the table points with
// the interpolation formula, never read back from the code under test.
// Ref: docs/decisions/0005-material-model.md items 9, 10; 0003 d8; pbrt-v4 PiecewiseLinearSpectrum.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "../../src/host/data_resource.h"
#include "../../src/host/ledger.h"
#include "../../src/host/light/light_builder.h"
#include "../../src/host/material/compiler.h"
#include "../../src/host/material/material_store.h"
#include "host/scene/edit.h"
#include "host/scene/mesh.h"
#include "host/scene/world.h"
#include "material/slot_resolve.h"
#include "vm/ops.h"
#include "vm/ops/store_slot.h"

namespace {

using ar::Op;
using astroray::host::compile_material;
using astroray::host::compile_volume;
using astroray::host::MaterialOutput;
using SK = ar::SpectrumKind;
using SS = ar::SpectrumSemantic;

const ar::Rgb2SpecTable& rgb_table() {
  static const auto store = std::make_shared<astroray::host::MaterialStore>(std::make_shared<astroray::host::Ledger>());
  return store->table();
}

const astroray::ObserverTables& observer() {
  static const astroray::ObserverTables t = [] {
    astroray::DataBlob blob;
    REQUIRE(astroray::find_data("spectral/observer.bin", &blob));
    astroray::ObserverTables o;
    const std::string err = astroray::parse_observer(blob.data, blob.size, &o);
    REQUIRE_MESSAGE(err.empty(), err);
    return o;
  }();
  return t;
}

ar::ObserverView view() { return ar::ObserverView{&observer().cmf[0][0], observer().d65}; }

// A table as the arena holds it: n wavelengths, then n values.
struct Pts {
  std::vector<float> w, v;
  std::vector<float> arena() const {
    std::vector<float> a(w);
    a.insert(a.end(), v.begin(), v.end());
    return a;
  }
  // The graph's f32 pool: scale, then (wavelength, value) pairs.
  std::vector<float> pool(float scale) const {
    std::vector<float> p{scale};
    for (size_t k = 0; k < w.size(); ++k) p.push_back(w[k]), p.push_back(v[k]);
    return p;
  }
};

// An irregular table of n points from `first` nm: spacing in [4, 9), values in [vmin, vmin + vspan)
// from a fixed recurrence.
Pts make_pts(uint32_t n, float first = 380.0f, float vmin = 0.05f, float vspan = 0.9f) {
  Pts p;
  uint32_t s = 12345u;
  float x = first;
  for (uint32_t k = 0; k < n; ++k) {
    s = s * 1664525u + 1013904223u;
    p.w.push_back(x);
    p.v.push_back(vmin + vspan * float(s >> 8) / 16777216.0f);
    x += 4.0f + 5.0f * float((s >> 4) & 0xffu) / 256.0f;
  }
  return p;
}

ar::Lambda lanes(float a, float b, float c, float d) {
  ar::Lambda l;
  l.nm[0] = a, l.nm[1] = b, l.nm[2] = c, l.nm[3] = d;
  return l;
}

// Piecewise-linear value in double: the formula typed from the definition, with the edge clamp.
double lerp_ref(const Pts& p, double x) {
  const size_t n = p.w.size();
  if (x <= double(p.w[0])) return double(p.v[0]);
  if (x >= double(p.w[n - 1])) return double(p.v[n - 1]);
  size_t k = 0;
  while (double(p.w[k + 1]) <= x) ++k;
  const double t = (x - double(p.w[k])) / (double(p.w[k + 1]) - double(p.w[k]));
  return double(p.v[k]) + t * (double(p.v[k + 1]) - double(p.v[k]));
}

// An ar_graph_desc built as the addon would.
struct GB {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  std::vector<int32_t> i;
  int add(Op op, std::vector<float> fp = {}, std::vector<int32_t> ip = {}) {
    ar_graph_node n = {};
    n.op = uint32_t(op);
    n.f32_first = uint32_t(f.size()), n.f32_count = uint32_t(fp.size());
    n.i32_first = uint32_t(i.size()), n.i32_count = uint32_t(ip.size());
    f.insert(f.end(), fp.begin(), fp.end());
    i.insert(i.end(), ip.begin(), ip.end());
    nodes.push_back(n);
    return int(nodes.size()) - 1;
  }
  void link(int from, int to, uint32_t socket) { links.push_back({uint32_t(from), 0u, uint32_t(to), socket}); }
  int store(int src, ar::RecordSlot s, int child = -1) {
    const int n = child < 0 ? add(Op::STORE_SLOT, {}, {int32_t(s)}) : add(Op::STORE_SLOT, {}, {int32_t(s), child});
    link(src, n, 0);
    return n;
  }
  int emit(int colour, int strength, int child = -1) {
    const int n = child < 0 ? add(Op::EMIT_LE) : add(Op::EMIT_LE, {}, {child});
    link(colour, n, 0);
    link(strength, n, 1);
    return n;
  }
  int table(const Pts& p, float scale = 1.0f) { return add(Op::SPECTRUM_TABLE, p.pool(scale)); }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size());
    d.f32_count = uint32_t(f.size()), d.i32_count = uint32_t(i.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data(), d.params_i32 = i.data();
    return d;
  }
  MaterialOutput compile() const { return compile_material(desc(), rgb_table()); }
};

size_t count(const MaterialOutput& m, uint32_t code) {
  return size_t(std::count_if(m.entries.begin(), m.entries.end(), [&](const auto& e) { return e.code == code; }));
}

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("spectrum_table/lookup at grid points") {
  // Every grid point of tables of 2, 3, 5, 64 and 4096 points returns scale x its value bit for bit,
  // the first and last included (a grid point is t = 0 or t = 1 exactly, so there is no rounding).
  for (uint32_t n : {2u, 3u, 5u, 64u, 4096u}) {
    const Pts p = make_pts(n);
    const std::vector<float> arena = p.arena();
    const ar::SpectrumRef r = ar::make_table_ref(SS::UNBOUNDED, 0u, n, 0.75f);
    uint32_t bad = 0;
    for (uint32_t k = 0; k < n; ++k) {
      const float a = p.w[k], b = p.w[(k + 1) % n], c = p.w[(k + 2) % n], d = p.w[n - 1 - k];
      const ar::Spec s = ar::table_spectrum(arena.data(), r, lanes(a, b, c, d));
      const uint32_t at[4] = {k, (k + 1) % n, (k + 2) % n, n - 1 - k};
      for (int l = 0; l < 4; ++l) {
        const float want = 0.75f * p.v[at[l]];
        bad += std::memcmp(&s.v[l], &want, sizeof want) != 0;
      }
    }
    CHECK_MESSAGE(bad == 0u, n << " points: " << bad << " grid values differ");
  }
}

TEST_CASE("spectrum_table/linear between points") {
  // 1e4 wavelengths strictly inside the support, against the double formula; the float blend has a
  // few ulps of error, the task allows 1e-6 relative. Values in [0.4, 0.9) keep a slope error of
  // 0.5 x 6e-8 well inside that relative to the value.
  const Pts p = make_pts(64, 380.0f, 0.4f, 0.5f);
  const std::vector<float> arena = p.arena();
  const ar::SpectrumRef r = ar::make_table_ref(SS::UNBOUNDED, 0u, 64u, 1.0f);
  uint32_t s = 99u;
  double worst = 0.0;
  for (uint32_t i = 0; i < 10000u; i += 4) {
    float x[4];
    for (float& v : x) {
      s = s * 1664525u + 1013904223u;
      v = p.w.front() + (p.w.back() - p.w.front()) * float(s >> 8) / 16777216.0f;
    }
    const ar::Spec got = ar::table_spectrum(arena.data(), r, lanes(x[0], x[1], x[2], x[3]));
    for (int l = 0; l < 4; ++l) {
      const double want = lerp_ref(p, double(x[l]));
      worst = std::max(worst, std::fabs(double(got.v[l]) - want) / want);
    }
  }
  CHECK(worst <= 1e-6);
  // A midpoint of the first segment is the mean of its ends.
  const float mid = 0.5f * (p.w[0] + p.w[1]);
  CHECK(double(ar::table_spectrum(arena.data(), r, lanes(mid, mid, mid, mid)).v[0]) ==
        doctest::Approx(0.5 * (double(p.v[0]) + double(p.v[1]))).epsilon(1e-6));
}

TEST_CASE("spectrum_table/end values outside the support") {
  const Pts p = make_pts(8);
  const std::vector<float> arena = p.arena();
  const ar::SpectrumRef r = ar::make_table_ref(SS::UNBOUNDED, 0u, 8u, 2.0f);
  const float lo = p.w.front(), hi = p.w.back();
  const ar::Spec s = ar::table_spectrum(arena.data(), r, lanes(lo - 1.0f, hi + 1.0f, -1e6f, 1e9f));
  CHECK(s.v[0] == 2.0f * p.v.front());
  CHECK(s.v[1] == 2.0f * p.v.back());
  CHECK(s.v[2] == 2.0f * p.v.front());
  CHECK(s.v[3] == 2.0f * p.v.back());
}

TEST_CASE("spectrum_table/semantics") {
  const ar::ObserverView obs = view();
  const Pts p{{360.0f, 830.0f}, {0.6f, 0.9f}};
  const std::vector<float> arena = p.arena();
  const ar::Lambda lam = lanes(400.0f, 500.0f, 600.0f, 700.0f);
  // REFLECTANCE clamps after the scale: 1.5 x 0.6 .. 1.5 x 0.9 is above 1 at 600 and 700 nm.
  const ar::Spec refl = ar::resolve(ar::make_table_ref(SS::REFLECTANCE, 0u, 2u, 1.5f), lam, obs, arena.data());
  for (int l = 0; l < 4; ++l) {
    const double want = std::min(1.0, 1.5 * lerp_ref(p, double(lam.nm[l])));
    CHECK(double(refl.v[l]) == doctest::Approx(want).epsilon(1e-6));
  }
  CHECK(refl.v[3] == 1.0f);
  // UNBOUNDED does not clamp.
  const ar::Spec unb = ar::resolve(ar::make_table_ref(SS::UNBOUNDED, 0u, 2u, 1.5f), lam, obs, arena.data());
  CHECK(double(unb.v[3]) == doctest::Approx(1.5 * lerp_ref(p, 700.0)).epsilon(1e-6));
  CHECK(unb.v[3] > 1.0f);
  // ILLUMINANT is the unbounded table times the observer's illuminant, like RGB and CONSTANT.
  const ar::Spec ill = ar::resolve(ar::make_table_ref(SS::ILLUMINANT, 0u, 2u, 2.0f), lam, obs, arena.data());
  for (int l = 0; l < 4; ++l) {
    const double want = 2.0 * lerp_ref(p, double(lam.nm[l])) * double(ar::illuminant_at(obs, lam.nm[l]));
    CHECK(double(ill.v[l]) == doctest::Approx(want).epsilon(1e-5));
  }
}

TEST_CASE("spectrum_table/graph refusals") {
  const Pts good = make_pts(4);
  auto refused = [&](std::vector<float> pool, const char* why, const char* what) {
    GB g;
    g.store(g.add(Op::SPECTRUM_TABLE, std::move(pool)), ar::RecordSlot::base_color);
    const MaterialOutput m = g.compile();
    INFO(what);
    CHECK(m.magenta);
    REQUIRE(m.entries.size() == 1);
    CHECK(m.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
    CHECK(m.entries[0].detail.find(why) != std::string::npos);
  };
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  const char* count_text = "SPECTRUM_TABLE needs a scale and 2 to 4096 wavelength and value pairs";
  refused({}, count_text, "no parameters");
  refused({1.0f, 500.0f, 0.5f}, count_text, "one point");
  refused({1.0f, 500.0f, 0.5f, 600.0f}, count_text, "an odd tail");
  std::vector<float> many{1.0f};
  for (int k = 0; k < 4097; ++k) many.push_back(300.0f + float(k)), many.push_back(0.5f);
  refused(many, count_text, "4097 points");
  refused({1.0f, 600.0f, 0.5f, 500.0f, 0.6f}, "strictly increasing", "unsorted");
  refused({1.0f, 500.0f, 0.5f, 500.0f, 0.6f}, "strictly increasing", "equal wavelengths");
  refused({1.0f, 500.0f, 0.5f, nan, 0.6f}, "finite and strictly increasing", "NaN wavelength");
  refused({1.0f, 500.0f, 0.5f, inf, 0.6f}, "finite and strictly increasing", "infinite wavelength");
  refused({1.0f, 500.0f, -0.1f, 600.0f, 0.6f}, "values must be finite and not negative", "negative value");
  refused({1.0f, 500.0f, 0.5f, 600.0f, nan}, "values must be finite and not negative", "NaN value");
  refused({-1.0f, 500.0f, 0.5f, 600.0f, 0.6f}, "finite non-negative scale", "negative scale");
  refused({inf, 500.0f, 0.5f, 600.0f, 0.6f}, "finite non-negative scale", "infinite scale");
  refused({1e30f, 500.0f, 0.5f, 600.0f, 1e10f}, "scale times a value overflows a float", "scale x value past FLT_MAX");
  // The limits themselves are accepted.
  for (uint32_t n : {2u, 4096u}) {
    GB g;
    g.store(g.table(make_pts(n)), ar::RecordSlot::base_color);
    const MaterialOutput m = g.compile();
    CHECK_MESSAGE(!m.magenta, n << " points");
    CHECK(m.compiled.block.base_color.aux == n);
  }
  // Where a table may go: a colour slot or the colour of an emission, nowhere else.
  auto misplaced = [&](const std::function<void(GB&, int)>& wire, const char* what) {
    GB g;
    wire(g, g.table(good));
    const MaterialOutput m = g.compile();
    INFO(what);
    CHECK(m.magenta);
    REQUIRE(m.entries.size() == 1);
    CHECK(m.entries[0].detail.find("SPECTRUM_TABLE only feeds a colour slot or the colour of an emission") != std::string::npos);
  };
  misplaced([](GB& g, int t) { g.store(t, ar::RecordSlot::base_weight); }, "a scalar slot");
  misplaced([](GB& g, int t) { g.store(t, ar::RecordSlot::specular_ior); }, "the IOR");
  misplaced([](GB& g, int t) { g.store(t, ar::RecordSlot::geometry_normal); }, "a frame slot");
  misplaced([](GB& g, int t) { g.emit(g.add(Op::CONST_F32X3, {1, 1, 1}), t); }, "the emission strength");
}

TEST_CASE("spectrum_table/arena is deduplicated within a material") {
  const Pts a = make_pts(5), b = make_pts(7, 400.0f);
  GB g;
  const int t1 = g.table(a, 0.9f), t2 = g.table(a, 0.4f), t3 = g.table(b);
  g.store(t1, ar::RecordSlot::base_color);
  g.store(t2, ar::RecordSlot::coat_color);
  g.store(t3, ar::RecordSlot::fuzz_color);
  const MaterialOutput m = g.compile();
  REQUIRE(!m.magenta);
  const ar::ConstantBlock& blk = m.compiled.block;
  // a once (two scales over one copy), then b: 2 x 5 + 2 x 7 floats.
  CHECK(m.compiled.spectra.size() == 24u);
  CHECK(blk.base_color.index == 0u);
  CHECK(blk.coat_color.index == 0u);
  CHECK(blk.fuzz_color.index == 10u);
  CHECK(blk.base_color.p[0] == 0.9f);
  CHECK(blk.coat_color.p[0] == 0.4f);
  CHECK(blk.fuzz_color.aux == 7u);
  CHECK(blk.base_color.semantic == uint8_t(SS::REFLECTANCE));
  const std::vector<float> aa = a.arena(), ba = b.arena();
  CHECK(std::equal(aa.begin(), aa.end(), m.compiled.spectra.begin()));
  CHECK(std::equal(ba.begin(), ba.end(), m.compiled.spectra.begin() + 10));
  // The UNBOUNDED slot takes a table too.
  GB u;
  u.store(u.table(a), ar::RecordSlot::subsurface_radius_scale);
  const MaterialOutput mu = u.compile();
  REQUIRE(!mu.magenta);
  CHECK(mu.compiled.block.subsurface_radius_scale.semantic == uint8_t(SS::UNBOUNDED));
  CHECK(mu.compiled.block.subsurface_radius_scale.kind == uint8_t(SK::TABLE));
}

TEST_CASE("spectrum_table/report entries once per material") {
  // Support [400, 700] does not cover [360, 830]: one SPECTRUM_OUT_OF_RANGE for the material, even
  // with two such tables and two slots reading one of them; a table that covers the range gives none.
  const Pts narrow{{400.0f, 700.0f}, {0.2f, 0.4f}}, narrower{{450.0f, 600.0f}, {0.2f, 0.4f}}, full{{360.0f, 830.0f}, {0.2f, 0.4f}};
  GB g;
  const int t1 = g.table(narrow);
  g.store(t1, ar::RecordSlot::base_color);
  g.store(t1, ar::RecordSlot::specular_color);
  g.store(g.table(narrower), ar::RecordSlot::coat_color);
  const MaterialOutput m = g.compile();
  REQUIRE(!m.magenta);
  CHECK(count(m, AR_REPORT_SPECTRUM_OUT_OF_RANGE) == 1u);
  CHECK(count(m, AR_REPORT_NODE_APPROXIMATED) == 0u);
  for (const auto& e : m.entries)
    if (e.code == AR_REPORT_SPECTRUM_OUT_OF_RANGE) {
      CHECK(e.detail.find("[400, 700] nm") != std::string::npos);
      CHECK(e.detail.find("[450, 600] nm") != std::string::npos);
      CHECK(e.detail.find("[360, 830]") != std::string::npos);
    }
  GB c;
  c.store(c.table(full), ar::RecordSlot::base_color);
  CHECK(c.compile().entries.empty());
  // A reflectance above 1 after the scale is clamped by the kernel and reported once, however many
  // slots; the same table in an UNBOUNDED slot is not.
  GB r;
  r.store(r.table(full, 3.0f), ar::RecordSlot::base_color);
  r.store(r.table(full, 5.0f), ar::RecordSlot::coat_color);
  const MaterialOutput mr = r.compile();
  CHECK(count(mr, AR_REPORT_NODE_APPROXIMATED) == 1u);
  GB ub;
  ub.store(ub.table(full, 3.0f), ar::RecordSlot::subsurface_radius_scale);
  CHECK(ub.compile().entries.empty());
}

TEST_CASE("spectrum_table/emission") {
  // One constant-strength TABLE emission is a block ref with the illuminant semantic and the strength
  // in the luminance; the estimate is a constant term at the table's mean.
  const Pts flat{{360.0f, 830.0f}, {0.5f, 0.5f}};
  GB g;
  g.emit(g.table(flat, 0.8f), g.add(Op::CONST_F32, {2.0f}));
  const MaterialOutput m = g.compile();
  REQUIRE(!m.magenta);
  const ar::SpectrumRef& r = m.compiled.block.emission_color;
  CHECK(r.kind == uint8_t(SK::TABLE));
  CHECK(r.semantic == uint8_t(SS::ILLUMINANT));
  CHECK(m.compiled.block.emission_luminance == 2.0f);
  REQUIRE(m.emission_estimate.size() == 1u);
  CHECK(m.emission_estimate[0].color.kind == uint8_t(SK::CONSTANT));
  CHECK(double(m.emission_estimate[0].color.p[0]) == doctest::Approx(0.4).epsilon(1e-6));
  CHECK(m.emission_estimate[0].strength == 2.0f);
  const ar::ObserverView obs = view();
  const ar::Lambda lam = lanes(420.0f, 520.0f, 620.0f, 720.0f);
  const ar::Spec le = ar::resolve(r, lam, obs, m.compiled.spectra.data());
  for (int l = 0; l < 4; ++l)
    CHECK(double(le.v[l]) == doctest::Approx(0.4 * double(ar::illuminant_at(obs, lam.nm[l]))).epsilon(1e-5));
  // The strength times the table must stay a float.
  GB big;
  big.emit(big.table(Pts{{360.0f, 830.0f}, {1e20f, 1e20f}}), big.add(Op::CONST_F32, {1e20f}));
  const MaterialOutput mb = big.compile();
  REQUIRE(mb.magenta);
  CHECK(mb.entries[0].detail.find("the scale, the values and the strength overflow a float") != std::string::npos);
  // Two emissions, a per-point strength and a per-point opacity cannot be one block ref.
  GB two;
  const int tt = two.table(flat);
  two.emit(tt, two.add(Op::CONST_F32, {1.0f}));
  two.emit(two.add(Op::CONST_F32X3, {1, 1, 1}), two.add(Op::CONST_F32, {1.0f}));
  const MaterialOutput m2 = two.compile();
  REQUIRE(m2.magenta);
  CHECK(m2.entries[0].detail.find("a TABLE emission must be the material's only emission") != std::string::npos);
  GB per;
  const int uv = per.add(Op::LOAD_CTX_UV, {}, {0});
  const int tex = per.add(Op::TEX_IMAGE, {}, {7, 0});
  per.link(uv, tex, 0);
  const int ch = per.add(Op::CHANNEL_F32X3, {}, {1});
  per.link(tex, ch, 0);
  per.emit(per.table(flat), ch);
  const MaterialOutput m3 = per.compile();
  REQUIRE(m3.magenta);
  CHECK(m3.entries[0].detail.find("the emission strength must be constant") != std::string::npos);
  GB op;
  op.emit(op.table(flat), op.add(Op::CONST_F32, {1.0f}));
  const int uv2 = op.add(Op::LOAD_CTX_UV, {}, {0});
  const int tex2 = op.add(Op::TEX_IMAGE, {}, {7, 0});
  op.link(uv2, tex2, 0);
  const int ch2 = op.add(Op::CHANNEL_F32X3, {}, {1});
  op.link(tex2, ch2, 0);
  op.store(ch2, ar::RecordSlot::geometry_opacity);
  const MaterialOutput m4 = op.compile();
  REQUIRE(m4.magenta);
  CHECK(m4.entries[0].detail.find("a TABLE emission needs a constant geometry_opacity") != std::string::npos);
}

TEST_CASE("spectrum_table/refused in a Mix Shader child and in a volume") {
  const Pts p = make_pts(4);
  GB mix;  // child 0 stores the table, child 1 a roughness; the factor is a constant
  const int factor = mix.add(Op::CONST_F32, {0.5f});
  const int bm = mix.add(Op::BRANCH_MIX, {}, {0, 0, 1});
  mix.link(factor, bm, 0);
  mix.store(mix.table(p), ar::RecordSlot::base_color, 0);
  mix.store(mix.add(Op::CONST_F32, {0.2f}), ar::RecordSlot::specular_roughness, 1);
  const MaterialOutput m = mix.compile();
  REQUIRE(m.magenta);
  CHECK(m.entries[0].detail.find("a TABLE spectrum cannot be stored per Mix Shader child") != std::string::npos);
  // An emission child of a Mix Shader cannot be a block ref either.
  GB me;
  const int f2 = me.add(Op::CONST_F32, {0.5f});
  const int bm2 = me.add(Op::BRANCH_MIX, {}, {0, 0, 1});
  me.link(f2, bm2, 0);
  me.emit(me.table(p), me.add(Op::CONST_F32, {1.0f}), 0);
  me.store(me.add(Op::CONST_F32, {0.2f}), ar::RecordSlot::specular_roughness, 1);
  const MaterialOutput mem = me.compile();
  REQUIRE(mem.magenta);
  CHECK(mem.entries[0].detail.find("a TABLE emission cannot sit under a Mix Shader") != std::string::npos);
  // The op has no interval rule and is not volume reachable: a volume graph that holds one is refused.
  GB vol;
  vol.table(p);
  const astroray::host::VolumeOutput v = compile_volume(vol.desc(), rgb_table());
  CHECK_FALSE(v.has_medium);
  REQUIRE(v.entries.size() == 1u);
  CHECK(v.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
  CHECK(v.entries[0].detail.find("the op has no volume meaning") != std::string::npos);
}

TEST_CASE("spectrum_table/a TABLE world colour is refused") {
  // A world's colour is a light record's: it reads no arena and would resolve to black, so the light
  // gather refuses it instead.
  astroray::host::MeshStore meshes;
  const astroray::host::MaterialArrays mats;
  astroray::host::WorldOutput world;
  world.color = ar::make_table_ref(SS::ILLUMINANT, 0u, 2u, 1.0f);
  world.strength = 1.0f;
  CHECK_THROWS_AS(astroray::host::gather_scene_lights(nullptr, meshes, mats, &world, true), std::invalid_argument);
  world.color = ar::make_const_ref(SS::ILLUMINANT, 1.0f);  // a constant colour is not
  CHECK_NOTHROW(astroray::host::gather_scene_lights(nullptr, meshes, mats, &world, true));
}

TEST_CASE("world/table colour is refused at compile") {
  // The world graph's one constant emission is a TABLE: compile_world renders the world magenta with
  // one entry, so the scene's light gather (which throws on a TABLE colour) never sees it.
  const Pts flat{{360.0f, 830.0f}, {0.5f, 0.5f}};
  GB g;
  g.emit(g.table(flat, 0.8f), g.add(Op::CONST_F32, {2.0f}));
  astroray::host::StoredGraph sg;
  sg.nodes = g.nodes, sg.links = g.links, sg.f32 = g.f, sg.i32 = g.i;
  ar_world_desc d = {};
  d.surface_graph = 1;
  const astroray::host::WorldOutput w = astroray::host::compile_world(d, &sg, rgb_table());
  CHECK(w.magenta);
  CHECK(w.color.kind != uint8_t(SK::TABLE));
  CHECK(w.program.empty());
  REQUIRE(w.entries.size() == 1);
  CHECK(w.entries[0].code == AR_REPORT_NODE_UNSUPPORTED);
  CHECK(w.entries[0].detail.find("SPECTRUM_TABLE") != std::string::npos);
  astroray::host::MeshStore meshes;
  const astroray::host::MaterialArrays mats;
  CHECK_NOTHROW(astroray::host::gather_scene_lights(nullptr, meshes, mats, &w, true));
}

TEST_CASE("compiler/spectrum table arena") {
  // Three materials in key order: A holds table T1, B holds T2, C holds T1 again. The scene arena is
  // T1 then T2, the refs are relocated from their material's own arena (B's was at 0), and C shares
  // A's copy.
  using namespace astroray::host;
  const Pts t1 = make_pts(3), t2 = make_pts(4, 420.0f);
  const ar_key GA = 10, GB_ = 11, GC = 12, A = 20, B = 21, C = 22;
  astroray::ContextState ctx;
  Scene scene(ctx, SceneConfig{2, {}});
  auto store = std::make_shared<MaterialStore>(std::make_shared<Ledger>());
  auto graph = [&](const Pts& p, float scale) {
    auto g = std::make_unique<GB>();
    g->store(g->table(p, scale), ar::RecordSlot::base_color);
    return g;
  };
  const auto ga = graph(t1, 0.5f), gb = graph(t2, 0.6f), gc = graph(t1, 0.7f);
  auto e = scene.begin_edit();
  REQUIRE(e);
  const struct {
    ar_key gkey, mkey;
    const GB* g;
  } items[3] = {{GA, A, ga.get()}, {GB_, B, gb.get()}, {GC, C, gc.get()}};
  for (const auto& it : items) {
    const ar_graph_desc d = it.g->desc();
    REQUIRE(store->set_graph(*e, it.gkey, &d) == AR_OK);
    ar_material_desc m = {};
    m.surface_graph = it.gkey;
    REQUIRE(store->set_material(*e, it.mkey, &m) == AR_OK);
  }
  uint64_t gen = 0;
  REQUIRE(e->commit(&gen) == AR_OK);
  store->end_edit();
  scene.generations().wait_idle();
  REQUIRE(scene.generations().failed() == 0);
  const auto arrays = store->arrays();
  REQUIRE(arrays);
  REQUIRE(arrays->keys == std::vector<ar_key>{A, B, C});
  std::vector<float> want = t1.arena();
  const std::vector<float> w2 = t2.arena();
  want.insert(want.end(), w2.begin(), w2.end());
  CHECK(arrays->spectra == want);  // 3 + 4 points, 2 floats each, T1 once
  CHECK(arrays->spectra.size() == 14u);
  const ar::ConstantBlock& ba = arrays->constants[0];
  const ar::ConstantBlock& bb = arrays->constants[1];
  const ar::ConstantBlock& bc = arrays->constants[2];
  CHECK((ba.base_color.index == 0u && ba.base_color.aux == 3u));
  CHECK((bb.base_color.index == 6u && bb.base_color.aux == 4u));
  CHECK((bc.base_color.index == 0u && bc.base_color.aux == 3u));
  CHECK(bb.base_color.p[0] == 0.6f);
  // The default material, last, has no table; the arena counts toward the ledger's bytes.
  CHECK(arrays->constants.back().base_color.kind == uint8_t(SK::RGB));
  CHECK(arrays->bytes() >= arrays->spectra.size() * sizeof(float));
  // What a kernel reads: B's record colour at a grid point of T2 is its scale times that value.
  const ar::Spec s = ar::resolve(bb.base_color, lanes(t2.w[0], t2.w[1], t2.w[2], t2.w[3]), view(), arrays->spectra.data());
  for (int l = 0; l < 4; ++l) CHECK(s.v[l] == 0.6f * t2.v[size_t(l)]);
}

}  // TEST_SUITE
