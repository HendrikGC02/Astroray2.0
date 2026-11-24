// SPDX-License-Identifier: Apache-2.0
// 0005 item 43 and Verification 11: MAY_BE_DELTA is never false when a delta lobe can be sampled, and
// the emission estimate is empty only when the emission folds to 0. 1e4 seeded graphs each, built from
// constants, TEX_IMAGE and arithmetic feeding the roughness, coat and Mix slots. The generator keeps
// its own ground truth as value ranges (a constant is a point, an image channel spans its texels, a UV
// channel spans [0, 1]) and never asks the compiler what a graph means. The estimate cases keep
// every texture mean above 0 and every multiplier above 0, so the only exact zeros are the folded
// constants the item names.
// Ref: docs/decisions/0005-material-model.md item 43 and Verification 11; 0004 D9 (an estimate of 0
// gives pmf 0); material/lobes/delta.h (the 1e-3 threshold on alpha = roughness^2).
#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../../src/host/ledger.h"
#include "../../src/host/material/compiler.h"
#include "../../src/host/material/graph.h"
#include "../../src/host/material/material_store.h"
#include "material/material_header.h"
#include "vm/interp.h"

namespace {

using ar::Op;
using ar::RecordSlot;
using astroray::host::mix_ref;

const ar::Rgb2SpecTable& table() {
  static const auto store = std::make_shared<astroray::host::MaterialStore>(std::make_shared<astroray::host::Ledger>());
  return store->table();
}

// The image bank: key 10 + k. `lo` and `hi` are the per-channel extremes over the texels, `mean` the
// linear-RGB mean the compiler is told. Every mean is above 0.
struct BankImage {
  std::array<double, 3> lo, hi, mean;
};
const BankImage kBank[] = {
    {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, {0.5, 0.5, 0.5}},      {{0.05, 0.0, 0.2}, {1.0, 0.4, 0.9}, {0.6, 0.2, 0.5}},
    {{0.4, 0.5, 0.6}, {0.9, 0.8, 1.0}, {0.7, 0.65, 0.8}},     {{0.0, 0.3, 0.0}, {0.9, 1.0, 0.2}, {0.4, 0.7, 0.1}},
    {{0.01, 0.02, 0.5}, {0.2, 0.3, 0.9}, {0.1, 0.15, 0.7}},   {{0.2, 0.2, 0.2}, {0.3, 0.3, 0.3}, {0.25, 0.25, 0.25}},
};
constexpr uint32_t kBankSize = uint32_t(sizeof kBank / sizeof kBank[0]);
constexpr uint32_t kBankKey = 10;

astroray::host::ImageMeanFn bank_means() {
  return [](uint32_t key, std::array<float, 3>* rgb) {
    if (key < kBankKey || key >= kBankKey + kBankSize) return false;
    for (int c = 0; c < 3; ++c) (*rgb)[size_t(c)] = float(kBank[key - kBankKey].mean[size_t(c)]);
    return true;
  };
}
astroray::host::ImageSlotFn bank_slots() {
  return [](uint32_t key, uint32_t* slot) {
    if (key < kBankKey || key >= kBankKey + kBankSize) return false;
    *slot = key - kBankKey;
    return true;
  };
}

struct Rng {
  uint64_t s;
  uint64_t next() {  // splitmix64
    uint64_t z = (s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }
  uint32_t below(uint32_t n) { return uint32_t((next() >> 32) % n); }
  bool chance(uint32_t percent) { return below(100) < percent; }
  template <size_t N>
  double pick(const double (&a)[N]) {
    return a[below(uint32_t(N))];
  }
};

// A value in the graph with the range of values it takes over the shading points.
struct Val {
  int node = -1;
  bool per_point = false;
  double lo = 0, hi = 0;
};

struct Gen {
  std::vector<ar_graph_node> nodes;
  std::vector<ar_graph_link> links;
  std::vector<float> f;
  std::vector<int32_t> i;
  Rng rng;

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
  int op2(Op op, int a, int b) {
    const int n = add(op);
    link(a, n, 0), link(b, n, 1);
    return n;
  }
  int store(int child, int src, RecordSlot s) {
    const int n = add(Op::STORE_SLOT, {}, {int32_t(s), child});
    link(src, n, 0);
    return n;
  }
  Val constant(double v) { return {add(Op::CONST_F32, {float(v)}), false, v, v}; }
  // Channel c of image k at UV set 0.
  Val tex_channel(uint32_t k, int c) {
    const int uv = add(Op::LOAD_CTX_UV, {}, {0});
    const int t = add(Op::TEX_IMAGE, {}, {int32_t(kBankKey + k), 0});
    link(uv, t, 0);
    const int ch = add(Op::CHANNEL_F32X3, {}, {c});
    link(t, ch, 0);
    return {ch, true, kBank[k].lo[size_t(c)], kBank[k].hi[size_t(c)]};
  }
  Val uv_channel(int c) {
    const int ch = add(Op::CHANNEL_F32X3, {}, {c});
    link(add(Op::LOAD_CTX_UV, {}, {0}), ch, 0);
    return {ch, true, 0.0, 1.0};
  }
  Val scaled(Val a, double m) {
    return {op2(Op::MUL_F32, a.node, add(Op::CONST_F32, {float(m)})), a.per_point, a.lo * m, a.hi * m};
  }
  // A scalar of one of the generator's shapes. `zero_ok`: a constant 0 may be drawn.
  Val scalar(bool zero_ok) {
    static const double consts[] = {0.0, 1e-4, 0.01, 0.02, 0.0316, 0.04, 0.1, 0.3, 0.5, 1.0};
    static const double mult[] = {0.5, 1.0, 2.0, 3.0};
    switch (rng.below(5)) {
      case 0:
      case 1: {
        double v = rng.pick(consts);
        if (v == 0.0 && !zero_ok) v = 0.3;
        return constant(v);
      }
      case 2: return tex_channel(rng.below(kBankSize), int(rng.below(3)));
      case 3: return scaled(tex_channel(rng.below(kBankSize), int(rng.below(3))), rng.pick(mult));
      default: return rng.chance(30) ? scaled(constant(rng.pick(consts)), rng.pick(mult)) : uv_channel(int(rng.below(2)));
    }
  }
  // An Emission child: a constant or textured colour and a scalar strength. `*zero` is set when the
  // emission folds to 0 on its own (a constant black colour or a constant strength 0).
  void emission(int child, bool* zero) {
    static const double chan[] = {0.0, 0.2, 0.5, 1.0};
    const int e = add(Op::EMIT_LE, {}, {child});
    bool colour_zero = false;
    if (rng.chance(60)) {
      const bool black = rng.chance(15);
      colour_zero = black;
      link(add(Op::CONST_F32X3, black ? std::vector<float>{0, 0, 0}
                                      : std::vector<float>{float(rng.pick(chan)), float(rng.pick(chan)), 0.5f}),
           e, 0);
    } else {
      const int uv = add(Op::LOAD_CTX_UV, {}, {0});
      const int t = add(Op::TEX_IMAGE, {}, {int32_t(kBankKey + rng.below(kBankSize)), 0});
      link(uv, t, 0);
      link(t, e, 0);
    }
    const Val strength = scalar(true);
    link(strength.node, e, 1);
    *zero = colour_zero || (!strength.per_point && strength.hi == 0.0);
  }
  ar_graph_desc desc() const {
    ar_graph_desc d = {};
    d.node_count = uint32_t(nodes.size()), d.link_count = uint32_t(links.size());
    d.f32_count = uint32_t(f.size()), d.i32_count = uint32_t(i.size());
    d.nodes = nodes.data(), d.links = links.data(), d.params_f32 = f.data(), d.params_i32 = i.data();
    return d;
  }
  astroray::host::MaterialOutput compile() const {
    const astroray::host::ImageMeanFn means = bank_means();
    const astroray::host::ImageSlotFn slots = bank_slots();
    return astroray::host::compile_material(desc(), table(), &means, &slots);
  }
};

constexpr double kDeltaRoughness2 = 1e-3;  // alpha = roughness^2 below this: a delta lobe (item 4)

bool alpha_delta(double roughness) { return roughness * roughness < kDeltaRoughness2; }

// Builds a Mix of `n` children (a chain: Mix(c0, Mix(c1, c2, f1), f0)) and returns each child's
// largest possible weight. n == 1 builds no Mix.
std::vector<double> build_mix(Gen* g, uint32_t n) {
  std::vector<double> hi(n, 1.0);
  if (n == 1) return hi;
  // Factors: a constant (0 and 1 force a child out), or a per-point UV channel.
  std::vector<Val> fac(n - 1);
  for (uint32_t k = 0; k + 1 < n; ++k) {
    static const double fc[] = {0.0, 0.25, 0.5, 0.75, 1.0};
    fac[k] = g->rng.chance(35) ? g->uv_channel(int(g->rng.below(2))) : g->constant(g->rng.pick(fc));
  }
  // Mix k selects child k (first input, weight 1 - f_k) or the rest of the chain (second input, f_k).
  double carry = 1.0;  // the largest weight of the subtree entering mix k
  for (uint32_t k = 0; k + 1 < n; ++k) {
    hi[k] = carry * (1.0 - fac[k].lo);
    carry *= fac[k].hi;
  }
  hi[n - 1] = carry;
  for (uint32_t k = 0; k + 1 < n; ++k) {
    const int32_t second = k + 2 < n ? mix_ref(int32_t(k + 1)) : int32_t(n - 1);
    const int m = g->add(Op::BRANCH_MIX, {}, {int32_t(k), int32_t(k), second});
    g->link(fac[k].node, m, 0);
  }
  return hi;
}

struct Counts {
  uint32_t graphs = 0, truth = 0, missed = 0, rough_graphs = 0, rough_flagged = 0, rejected = 0;
};

}  // namespace

TEST_SUITE("fast") {

TEST_CASE("compiler_flags/never false on generated graphs") {
  // Ref: 0005 Verification 11, item 43: MAY_BE_DELTA is true whenever a delta lobe can be sampled.
  Counts c;
  for (uint32_t seed = 0; seed < 10000; ++seed) {
    Gen g;
    g.rng.s = 0x51ed0000ull + seed;
    const uint32_t n = 1 + g.rng.below(3);
    const std::vector<double> weight_hi = build_mix(&g, n);
    const uint32_t mirror = n > 1 ? g.rng.below(n) : 0;
    bool truth = false, all_rough = true;
    for (uint32_t k = 0; k < n; ++k) {
      if (n > 1 && g.rng.chance(20)) {  // an Emission child has no lobes
        bool ignored = false;
        g.emission(int(k), &ignored);
        continue;
      }
      // Every child stores something, so a child with no roughness input still exists.
      g.store(int(k), g.constant(0.25).node, RecordSlot::base_metalness);
      const bool is_mirror = g.rng.chance(15) && k == mirror;
      Val rough = is_mirror ? g.constant(0.0) : g.scalar(true);
      const bool stored = is_mirror || g.rng.chance(85);  // an unstored roughness is the default 0.3
      if (stored) g.store(int(k), rough.node, RecordSlot::specular_roughness);
      Val coat_w = g.rng.chance(50) ? g.scalar(true) : Val{-1, false, 0.0, 0.0};
      if (coat_w.node >= 0) g.store(int(k), coat_w.node, RecordSlot::coat_weight);
      // An unstored coat roughness is the default 0.
      Val coat_r = g.rng.chance(60) ? g.scalar(true) : Val{-1, false, 0.0, 0.0};
      if (coat_r.node >= 0) g.store(int(k), coat_r.node, RecordSlot::coat_roughness);
      const bool spec_delta = alpha_delta(stored ? rough.lo : 0.3);
      const bool coat_delta = coat_w.hi > 0.0 && alpha_delta(coat_r.node >= 0 ? coat_r.lo : 0.0);
      if (weight_hi[k] > 0.0 && (spec_delta || coat_delta)) truth = true;
      // The roughness of every lobe that can be sampled is above 0.01 (the false-rate report).
      if (weight_hi[k] > 0.0) {
        if (stored ? rough.lo <= 0.01 : false) all_rough = false;
        if (coat_w.hi > 0.0 && (coat_r.node >= 0 ? coat_r.lo : 0.0) <= 0.01) all_rough = false;
      }
    }
    const astroray::host::MaterialOutput m = g.compile();
    ++c.graphs;
    if (m.magenta) {
      ++c.rejected;
      MESSAGE("seed " << seed << " compiled magenta: " << (m.entries.empty() ? "" : m.entries[0].detail));
      continue;
    }
    const bool flag = (m.flags & ar::MAY_BE_DELTA) != 0;
    if (truth) {
      ++c.truth;
      if (!flag) {
        ++c.missed;
        if (c.missed <= 5) MESSAGE("seed " << seed << ": a delta lobe can be sampled and MAY_BE_DELTA is false");
      }
    }
    if (all_rough) {
      ++c.rough_graphs;
      c.rough_flagged += flag;
    }
  }
  // Reported, not required: how often a graph whose lobes are all rougher than 0.01 is flagged.
  std::printf("compiler_flags: %u graphs, %u with a samplable delta lobe, %u missed; all-rough graphs flagged %u of %u\n",
              c.graphs, c.truth, c.missed, c.rough_flagged, c.rough_graphs);
  CHECK(c.rejected == 0);
  CHECK(c.truth > 1000);  // the generator exercises the rule
  CHECK(c.missed == 0);
}

TEST_CASE("compiler_flags/estimate zero only when folded") {
  // Ref: 0005 item 43: the estimate is 0 only when the emission folds to 0 (a constant black colour,
  // a constant strength 0 or a child weight of constant 0); anything per point is bounded or
  // measured above 0.
  uint32_t zero_graphs = 0, wrong_zero = 0, wrong_nonzero = 0, rejected = 0;
  for (uint32_t seed = 0; seed < 10000; ++seed) {
    Gen g;
    g.rng.s = 0xe1155100ull + seed;
    const uint32_t n = 1 + g.rng.below(3);
    const std::vector<double> weight_hi = build_mix(&g, n);
    bool any_emission = false;
    for (uint32_t k = 0; k < n; ++k) {
      if (g.rng.chance(60)) {
        bool zero = false;
        g.emission(int(k), &zero);
        any_emission = any_emission || (!zero && weight_hi[k] > 0.0);
      } else {
        g.store(int(k), g.constant(0.25).node, RecordSlot::base_metalness);
      }
    }
    const astroray::host::MaterialOutput m = g.compile();
    if (m.magenta) {
      MESSAGE("seed " << seed << " compiled magenta: " << (m.entries.empty() ? "" : m.entries[0].detail));
      ++rejected;
      continue;
    }
    if (!any_emission) ++zero_graphs;
    if (any_emission && m.emission_estimate.empty()) {
      if (++wrong_zero <= 5) MESSAGE("seed " << seed << ": the emission does not fold to 0 and the estimate is empty");
    }
    if (!any_emission && !m.emission_estimate.empty()) {
      if (++wrong_nonzero <= 5) MESSAGE("seed " << seed << ": the emission folds to 0 and the estimate has terms");
    }
  }
  std::printf("compiler_flags: %u graphs fold to 0; wrong zero %u, wrong non-zero %u\n", zero_graphs, wrong_zero, wrong_nonzero);
  CHECK(rejected == 0);
  CHECK(zero_graphs > 500);
  CHECK(wrong_zero == 0);
  CHECK(wrong_nonzero == 0);
}

TEST_CASE("compiler_flags/hand shapes") {
  using astroray::host::MaterialOutput;
  auto delta = [](const MaterialOutput& m) { return (m.flags & ar::MAY_BE_DELTA) != 0; };
  // Mix(mirror, rough, f): f = 1 gives the mirror a weight of 0 (the first input weighs 1 - f), so no
  // sample takes it; f = 0 takes it for certain; a per-point f can take it.
  auto mirror_mix = [](bool per_point, float f) {
    Gen g;
    g.store(0, g.constant(0.0).node, RecordSlot::specular_roughness);
    g.store(1, g.constant(0.5).node, RecordSlot::specular_roughness);
    const Val fv = per_point ? g.uv_channel(0) : g.constant(f);
    g.link(fv.node, g.add(Op::BRANCH_MIX, {}, {0, 0, 1}), 0);
    return g.compile();
  };
  CHECK_FALSE(delta(mirror_mix(false, 1.0f)));
  CHECK(delta(mirror_mix(false, 0.0f)));
  CHECK(delta(mirror_mix(false, 0.5f)));
  CHECK(delta(mirror_mix(true, 0.0f)));
  {  // a sampled coat of roughness 0 (the default) on the second child only
    Gen g;
    g.store(0, g.constant(0.5).node, RecordSlot::specular_roughness);
    g.store(1, g.constant(0.5).node, RecordSlot::specular_roughness);
    g.store(1, g.constant(1.0).node, RecordSlot::coat_weight);
    g.link(g.constant(0.5).node, g.add(Op::BRANCH_MIX, {}, {0, 0, 1}), 0);
    CHECK(delta(g.compile()));
  }
  {  // a coat weight from an image that is 0 at some texels, over the default coat roughness of 0
    Gen g;
    g.store(0, g.tex_channel(0, 0).node, RecordSlot::coat_weight);
    g.store(0, g.constant(0.5).node, RecordSlot::specular_roughness);
    CHECK(delta(g.compile()));
  }
  {  // a roughness that is a UV channel scaled by a constant: per point, so it may reach 0
    Gen g;
    g.store(0, g.scaled(g.uv_channel(1), 3.0).node, RecordSlot::specular_roughness);
    CHECK(delta(g.compile()));
  }
  {  // three emission children, the first two with a selection weight of constant 0: the estimate keeps
     // the third's term (each EMIT_LE needs its own strength value; the zero is one shared node)
    Gen g;
    for (int k = 0; k < 3; ++k) {
      const int e = g.add(Op::EMIT_LE, {}, {k});
      g.link(g.add(Op::CONST_F32X3, {0.5f, 0.5f, 0.5f}), e, 0);
      g.link(g.add(Op::CONST_F32, {k == 2 ? 3.0f : 2.0f}), e, 1);
    }
    g.link(g.constant(1.0).node, g.add(Op::BRANCH_MIX, {}, {0, 0, mix_ref(1)}), 0);
    g.link(g.constant(1.0).node, g.add(Op::BRANCH_MIX, {}, {1, 1, 2}), 0);
    const MaterialOutput m = g.compile();
    REQUIRE_FALSE(m.magenta);
    REQUIRE(m.emission_estimate.size() == 1);
    CHECK(m.emission_estimate[0].strength == 3.0f);
  }
}

TEST_CASE("compiler_flags/non finite constants are refused") {
  using astroray::host::MaterialOutput;
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  auto refused = [](const MaterialOutput& m, const std::string& what) {
    REQUIRE(m.magenta);
    REQUIRE(m.entries.size() == 1);
    CHECK(m.entries[0].code == AR_REPORT_PROGRAM_REJECTED);
    CHECK(m.entries[0].detail.find(what) != std::string::npos);
    CHECK(m.entries[0].detail.find("non-finite") != std::string::npos);
  };
  {  // a constant straight into a slot: the node and the slot are named
    Gen g;
    g.store(0, g.add(Op::CONST_F32, {nan}), RecordSlot::specular_roughness);
    refused(g.compile(), "record slot " + std::to_string(int(RecordSlot::specular_roughness)));
  }
  {  // an Inf colour
    Gen g;
    g.store(0, g.add(Op::CONST_F32X3, {0.5f, inf, 0.5f}), RecordSlot::base_color);
    refused(g.compile(), "STORE_SLOT");
  }
  {  // an Inf emission strength
    Gen g;
    const int e = g.add(Op::EMIT_LE, {}, {0});
    g.link(g.add(Op::CONST_F32X3, {1.0f, 1.0f, 1.0f}), e, 0);
    g.link(g.add(Op::CONST_F32, {inf}), e, 1);
    refused(g.compile(), "EMIT_LE");
  }
  {  // a NaN that only multiplies a per-point value
    Gen g;
    g.store(0, g.scaled(g.uv_channel(0), double(nan)).node, RecordSlot::specular_roughness);
    refused(g.compile(), "MUL_F32");
  }
  {  // a NaN under a Mix child
    Gen g;
    g.store(0, g.add(Op::CONST_F32, {nan}), RecordSlot::coat_weight);
    g.store(1, g.constant(0.5).node, RecordSlot::specular_roughness);
    g.link(g.constant(0.5).node, g.add(Op::BRANCH_MIX, {}, {0, 0, 1}), 0);
    refused(g.compile(), "STORE_SLOT");
  }
  {  // finite constants of the same shapes compile
    Gen g;
    g.store(0, g.add(Op::CONST_F32, {0.5f}), RecordSlot::specular_roughness);
    CHECK_FALSE(g.compile().magenta);
  }
}

}  // TEST_SUITE
